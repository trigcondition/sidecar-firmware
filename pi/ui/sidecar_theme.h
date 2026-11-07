// pi/ui/sidecar_theme.h - the Pi display's theme layer (docs/UI-THEMES.md): every colour, font, line style, spacing and
// widget of the display's pages comes from here. Two themes:
//   OLED (default)  the whole screen as a zoomed-in Elektron OLED: 400x240 dots on 800x480, 512x300 on 1024x600, 640x360 on 1280x720 (sidecar_dot.h), black, a
//                   pixel font of our own (sidecar_dotfont.h), dotted outlines, grey dithered title bars, inverted
//                   selection, knobs as dot rings, colour only from the Rytm's LED palette
//   SIDECAR         the 800x480 layout in real pixels, drawn in the web app's "UV night" palette: rounded cards with a
//                   pill label, pill keys and segmented controls (selected = filled), arc knobs, rounded meters, Space
//                   Grotesk for text and the dot font (round dots) for the big numbers; from 1024x600 the T1..T8 keys
//                   are a rail at the right edge, at 800x480 a row in the header
// The pages lay themselves out per theme (TH->id) and draw only through the widgets and painters below, so a new theme
// is a table and a set of painters (docs/UI-THEMES.md "Adding a theme"). LVGL only, one thread (core 0 on the Pi).
#ifndef SIDECAR_THEME_H
#define SIDECAR_THEME_H

#include "lvgl.h"
#include "sidecar_dot.h"

#ifdef __cplusplus
extern "C" {
#endif

// The ids are stored (display.txt uistyle=, the status's ui_style): SIDECAR took CLASSIC's 1, so a card that chose the
// old CLASSIC theme comes up in SIDECAR ("classic" is still read as 1).
enum { THEME_OLED = 0, THEME_SIDECAR = 1, THEME_N };

// colours the OLED theme's dot expansion turns into patterns (sidecar_dot_expand): exact RGB565 values that no
// drawing produces otherwise (1-bit fonts, square corners, no anti-aliasing in that theme)
#define DOT_DITHER	0x080808	// the title bar: light and mid grey in a checkerboard
#define DOT_DOTTED	0x080810	// a dotted line: every other dot grey
#define DOT_SHADE	0x080818	// a dark checkerboard: pressed, outside the pattern, inactive areas
#define DOT_ALERT	0x080820	// the power warning's bar: the accent and its tint in a checkerboard

typedef struct
{
	int		id;
	const char	*name;			// "OLED", "SIDECAR" (the settings page, display.txt's uistyle=)
	int		dot;			// physical pixels a UI pixel: SIDECAR_DOT or 1
	int		w, h;			// the UI's size in its own pixels now: OLED 400x240 / 512x300 / 640x360, SIDECAR 800x480 / 1024x600 / 1280x720
	int		bw, bh;			// the base layout's size in them (OLED 400x240, SIDECAR 800x480; 1280x720: LU of those)
	int		dx, dy;			// w - nav - bw (OLED: w - bw), h - bh: what the screen adds to the base layout (1024x600: OLED 112x60 dots, SIDECAR 128x120 px; 1280x720: 140x60, 160x120; 800x480: 0)
	int		nav;			// the navigation column at the right edge: OLED LU (48) (inside its base layout), SIDECAR's rail from 1024x600 on (96 / 120 px, outside it), else 0
	int		big;			// 1280x720: LU below
	// colours, 0xRRGGBB
	uint32_t	bg;			// the screen
	uint32_t	ink, dim, faint;	// text on the screen / panels: normal, secondary, disabled
	uint32_t	sub;			// hints, printed function labels (SIDECAR: the labels' lavender; OLED: dim)
	uint32_t	win, win_edge;		// a display window (SIDECAR: a card and its border; OLED: black)
	uint32_t	win_ink, win_dim;	// text inside a window
	uint32_t	title_bg, title_ink;	// a title bar (OLED: DOT_DITHER with black)
	uint32_t	sel_bg, sel_ink;	// a selection (OLED: inverted)
	uint32_t	accent, accent_hi;	// the one colour accent (OLED: an LED colour, sparingly)
	uint32_t	ok, bad, warn;		// connected / errors / held, locks
	uint32_t	play;			// play heads
	uint32_t	lock;			// p-locks (the Rytm view)
	// the piano roll and the Rytm grid
	uint32_t	row_in, row_out, row_root, line_step, line_beat, line_page, key_white, key_black, muted;
	// fonts: micro (rulers, captions), small, body, big (values), title
	const lv_font_t	*f_micro, *f_small, *f_body, *f_big, *f_title;
	const lv_font_t	*f_mid, *f_mid2;	// SIDECAR's two sizes between those (Space Grotesk 18, 22)
	const lv_font_t	*f_num, *f_hero;	// the dot font for numbers: a knob's value, the header's page name and HOME's pattern (OLED: f_big)
	// metrics in the theme's pixels
	int		radius;			// corners (OLED: 0)
	int		gap;			// between controls
	int		touch;			// the smallest touch target (about 40 physical px)
} sidecar_theme_t;

extern const sidecar_theme_t *g_Theme;
#define TH g_Theme
void sidecar_theme_set (int id);		// THEME_*; the pages are rebuilt by the caller (sidecar_ui_style)
const sidecar_theme_t *sidecar_theme_get (int id);	// the 800x480 table
// The screen's size (physical pixels, before sidecar_theme_set): 1280x720 or more in both gives the big layouts, 1024x600
// or more the wide ones (TH->w, h, dx, dy), anything else the 800x480 ones. The pages place what grows from TH->dx / dy
// (docs/UI-THEMES.md "Sizes").
void sidecar_theme_screen (int phys_w, int phys_h);
int sidecar_theme_wide (void);			// 1: the 1024x600 layouts or bigger
// A layout size, in the theme's pixels: n, or n x 5/4 at 1280x720. The whole base layout is written in these, so it comes
// out at 500x300 dots / 1000x600 px there, drawn at the real pixels with bigger faces and keys, and TH->dx, dy is the room
// left on top. Not a scale of the picture: a line stays one dot wide, a grid stays in whole dots.
#define LU(n)	(TH->big ? (n) * 5 / 4 : (n))

lv_color_t th_c (uint32_t rgb);				// lv_color_hex
uint32_t th_mix (uint32_t a, uint32_t b, int t256);	// a..b, t = 0..256
uint32_t th_led (int palette);				// SIDECAR: the palette's ramps in its own hues; OLED: white / teal / its tint
uint32_t th_oled_secondary (int vel);			// OLED: the panel's blue print by velocity (single notes, trigs)
uint32_t th_oled_accent (int vel);			// OLED: the FUNC teal by velocity (chords)

// ---- widgets: plain lv_obj's drawn by the theme's painters (DRAW_MAIN), their text and state kept by the theme
enum						// th_key kinds and flags
{
	TH_KEY = 0,				// a key: SIDECAR an outlined pill, OLED an outlined box
	TH_KEY_ACCENT = 1,			// the page's main action
	TH_KEY_DANGER = 2,			// delete, kill
	TH_KEY_FUNC = 3,			// a navigation / shift key (SIDECAR: tinted with the accent)
	TH_KEY_TRIG = 4,			// a trig key: a number with an LED
	TH_KEY_PAD = 5,				// a pad (SIDECAR: a rounded tile)
	TH_KEY_NAV = 6,				// the right-hand navigation column (OLED, SIDECAR's rail): an icon, its name, its T key
	TH_KEY_KIND = 7,
	TH_KEY_SMALL = 8,			// the micro / small font
	TH_KEY_LEFT = 16,			// text left-aligned (pads, lists)
	TH_KEY_HOLD = 32,			// checkable (seg items are)
	TH_KEY_FILL = 64,			// drawn filled (inverted): a dialog's destructive confirm (UX review T16)
};
typedef void (*th_cb_t) (lv_obj_t *o, int idx, void *ud);

lv_obj_t *th_key (lv_obj_t *parent, int x, int y, int w, int h, const char *text, int kind, th_cb_t cb, void *ud);
void th_key_text (lv_obj_t *k, const char *text);		// copied (23 chars)
void th_key_led (lv_obj_t *k, int palette);			// the key's LED: a palette colour (0: off)
void th_key_sub (lv_obj_t *k, const char *sub);			// a second, smaller line (pads: the machine)
void th_key_check (lv_obj_t *k, int on);
void th_key_enable (lv_obj_t *k, int on);
void th_key_font (lv_obj_t *k, const lv_font_t *f);		// 0: the kind's (TH_KEY_SMALL: micro, else body)
void th_key_kind (lv_obj_t *k, int kind);			// another kind / flags (a dialog's key that changes role)

// A segmented control: one item checked at a time (map: items, "\n" starts a row, "" ends it). cb(seg, idx, ud) on a
// tap (on release). Items take (w - gaps) / n each.
lv_obj_t *th_seg (lv_obj_t *parent, int x, int y, int w, int h, const char *const *map, int kind, th_cb_t cb, void *ud);
void th_seg_map (lv_obj_t *seg, const char *const *map);	// new item texts (the same layout rule)
void th_seg_set (lv_obj_t *seg, int idx);			// -1: none; only a change redraws
int th_seg_get (lv_obj_t *seg);
void th_seg_enable (lv_obj_t *seg, int idx, int on);		// idx -1: all
void th_seg_checkable (lv_obj_t *seg, int idx, int on);		// an item that acts, never stays checked ("...")
void th_seg_led (lv_obj_t *seg, int idx, int palette);
lv_obj_t *th_seg_item (lv_obj_t *seg, int idx);
int th_seg_count (lv_obj_t *seg);
void th_seg_gap (lv_obj_t *seg, int gap);
void th_seg_font (lv_obj_t *seg, const lv_font_t *f);

enum { TH_PANEL_GROUP = 0, TH_PANEL_WINDOW = 1, TH_PANEL_BARE = 2 };	// controls on the panel / a display window / no frame
lv_obj_t *th_panel (lv_obj_t *parent, int x, int y, int w, int h, const char *title, int kind);
int th_panel_top (void);					// where a panel's content starts below its title

// a title bar (OLED: the dithered bar of every page): bold text left, text right; alert: the power warning's look
lv_obj_t *th_titlebar (lv_obj_t *parent, int x, int y, int w, int h);
void th_titlebar_set (lv_obj_t *t, const char *left, const char *right, int alert);
void th_titlebar_led (lv_obj_t *t, int palette);		// a status LED at the right end (0: none)

lv_obj_t *th_label (lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t rgb, const char *text);
void th_label_set (lv_obj_t *l, const char *text);		// no redraw when unchanged
void th_label_color (lv_obj_t *l, uint32_t rgb);
// a name or a path into a label of fixed width: cut in the middle with ".." when it is wider (UX review R1: its start and
// its end both stay readable, "JUNO_A15..STRINGS"); else as th_label_set
void th_label_fit (lv_obj_t *l, const char *text);
// the same into a buffer for a painter: t cut in the middle to w pixels of f (out holds cap bytes)
void th_fit_middle (char *out, int cap, const lv_font_t *f, const char *t, int w);

enum { TH_METER_H = 0, TH_METER_V = 1, TH_METER_KNOB = 2 };
lv_obj_t *th_meter (lv_obj_t *parent, int x, int y, int w, int h, int max, int kind, uint32_t rgb);
void th_meter_set (lv_obj_t *m, int v);				// -1: not known
void th_meter_label (lv_obj_t *m, const char *caption);		// knobs: the caption under the ring
void th_meter_value (lv_obj_t *m, const char *value);		// knobs: the value under the caption ("" none; "---" not known)

// a slider: cb (obj, value, ud) while dragging, and once more on release with idx | TH_RELEASED
#define TH_RELEASED 0x10000
lv_obj_t *th_slider (lv_obj_t *parent, int x, int y, int w, int h, int min, int max, th_cb_t cb, void *ud);
void th_slider_set (lv_obj_t *s, int v);
int th_slider_get (lv_obj_t *s);
int th_slider_dragging (lv_obj_t *s);

// a plain object of the given size (no style, not scrollable): containers and code-drawn areas
lv_obj_t *th_box (lv_obj_t *parent, int x, int y, int w, int h);

// ---- painters for code-drawn areas (the piano roll, the Rytm grid): c is the object's coordinates, the rest relative
void th_fill (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, lv_opa_t opa, int r);
void th_frame (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, int w, int r);
void th_text (lv_layer_t *l, const lv_area_t *c, int x, int y, int w, const lv_font_t *f, uint32_t rgb, lv_text_align_t al,
	      const char *t);
// a key painted into an area (the page strip's cells, the Rytm grid's names): the same look as th_key
void th_paint_key (lv_layer_t *l, const lv_area_t *a, const char *text, const char *sub, const lv_font_t *f, int kind,
		   int checked, int pressed, int disabled, int led);
// a knob: v of max, the pointer and a ring of dots (OLED) or a 270 degree arc with a dot at the value (SIDECAR)
void th_paint_knob (lv_layer_t *l, const lv_area_t *a, int v, int max, uint32_t rgb);

int th_text_width (const lv_font_t *f, const char *t);
// SIDECAR's "Docked" logo (the web app's): a disc of dots in the palette's hues with a solid accent disc docked at its
// lower right, in a box of size x size*40/46 at x, y of the area (decoration only)
void th_paint_logo (lv_layer_t *l, const lv_area_t *c, int x, int y, int size);
// the width a page has: the screen less the navigation column (OLED LU (48), SIDECAR's rail)
int th_page_w (void);

// The text-fit check (UX review R1; host/shot.c): when set, every string a painter draws wider than its box, and a
// title bar whose left and right texts overlap, is reported (the host's shots fail on any). Null on the Pi.
typedef void (*th_fit_fn) (const char *text, int need, int have);
extern th_fit_fn th_fit_report;

#ifdef __cplusplus
}
#endif

#endif
