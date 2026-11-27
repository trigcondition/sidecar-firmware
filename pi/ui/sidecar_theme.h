// pi/ui/sidecar_theme.h - the Pi display's theme layer (docs/UI-THEMES.md): every colour, font, line style, spacing and
// widget of the display's pages comes from here. Three themes:
//   OLED (default)  the whole screen as a zoomed-in Elektron OLED: 400x240 dots on 800x480, 512x300 on 1024x600, 640x360 on 1280x720 (sidecar_dot.h), black, a
//                   pixel font of our own (sidecar_dotfont.h), dotted outlines, grey dithered title bars, inverted
//                   selection, knobs as dot rings, colour only from the Rytm's LED palette
//   SIDECAR         the web app's "UV night" palette (and its high-contrast option, blue or green) in GRID's layout language:
//                   frames with a small radius like the Rytm's pads and buttons, 2 px edges (3 px in high contrast), square-ended
//                   arc knobs, Arimo Bold in mixed case; from 1024x600 the T1..T8 keys are a rail at the right edge, at 800x480 a
//                   row in the header
//   GRID           the style guide's look: a dark army-green screen, signal green only for the active and the selected, sage
//                   notes, Arimo Bold in capitals (14 / 19 / 24 px and 44 for the hero numbers at 1280x720, smaller sets below), 2 px
//                   edges, panels and selector keys on one grid
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
// old CLASSIC theme comes up in SIDECAR ("classic" is still read as 1). The 0.5.1 test build's SQUARE (2) and GRID (3) are
// gone: both words and both numbers read as GRID (ui_style.h).
enum { THEME_OLED = 0, THEME_SIDECAR = 1, THEME_GRID = 2, THEME_N };
#define THEME_BOXED(id)	((id) != THEME_OLED)	// SIDECAR or GRID: frames, the small radius, Arimo Bold (the pages' layout for both)

// colours the OLED theme's dot expansion turns into patterns (sidecar_dot_expand): exact RGB565 values that no
// drawing produces otherwise (1-bit fonts, square corners, no anti-aliasing in that theme)
#define DOT_DITHER	0x080808	// the title bar: light and mid grey in a checkerboard
#define DOT_DOTTED	0x080810	// a dotted line: every other dot grey
#define DOT_SHADE	0x080818	// a dark checkerboard: pressed, outside the pattern, inactive areas
#define DOT_ALERT	0x080820	// the power warning's bar: the accent and its tint in a checkerboard

typedef struct
{
	int		id;
	const char	*name;			// "OLED", "SIDECAR", "GRID" (the settings page, display.txt's uistyle=)
	int		dot;			// physical pixels a UI pixel: SIDECAR_DOT or 1
	int		w, h;			// the UI's size in its own pixels now: OLED 400x240 / 512x300 / 640x360, SIDECAR 800x480 / 1024x600 / 1280x720
	int		bw, bh;			// the base layout's size in them (OLED 400x240, SIDECAR 800x480; 1280x720: LU of those)
	int		dx, dy;			// w - nav - bw (OLED: w - bw), h - bh: what the screen adds to the base layout (1024x600: OLED 112x60 dots, SIDECAR 128x120 px; 1280x720: 140x60, 160x120; 800x480: 0)
	int		nav;			// the navigation column at the right edge: OLED LU (48) (inside its base layout), SIDECAR's rail from 1024x600 on (96 / 120 px, outside it), else 0
	int		pdy;			// dy less what the sub-page row takes (SIDECAR 58 px, OLED 25 dots, in LU): what the pages below the row stretch by (0.5.1)
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
	const lv_font_t	*f_mid, *f_mid2;	// (the title face in SIDECAR and GRID)
	const lv_font_t	*f_num, *f_hero;	// numbers: a knob's value (the title face), HOME's pattern (the hero face) (OLED: f_big)
	// metrics in the theme's pixels
	int		radius;			// corners (OLED: 0)
	int		gap;			// between controls
	int		touch;			// the smallest touch target (about 40 physical px)
	int		edge_w;			// SIDECAR: a card's / control's border width (1; 2 in the high-contrast look)
	// SIDECAR's pixel accent (the bold round-dot 5x9 / 5x7 faces, integer scales; 0 in GRID and OLED): knob and tempo values,
	// the hero pattern, the small face (INSERT FX values, the header strip)
	const lv_font_t	*f_pxv, *f_pxh, *f_pxs;
	int		hc;			// SIDECAR: 0 standard, 1 high contrast with the blue accent, 2 with the green one, 3 Viper: phosphor greens (sidecar_theme_contrast)
} sidecar_theme_t;

extern const sidecar_theme_t *g_Theme;
#define TH g_Theme
// The contrast option of the SIDECAR theme: 0 off, 1 light blue accent, 2 Rytm mute-pad green, 3 Viper (phosphor greens). Stored for the next
// sidecar_theme_set (the OLED theme ignores it); the pages are rebuilt by the caller.
void sidecar_theme_contrast (int hc);
int sidecar_theme_get_contrast (void);
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
	TH_KEY_ROUND = 256,			// SIDECAR / GRID: a circle (a track selector; every other key is square there)
	TH_KEY_TAB = 128,			// a header page key: in SIDECAR's high contrast a tab (accent label, selected = accent fill, rounded top)
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
int th_alert_bar (lv_obj_t *l, lv_area_t *bar);	// Viper: 1 and the area of the alert bar behind a label (the host's check), else 0
// a name or a path into a label of fixed width: cut in the middle with ".." when it is wider (UX review R1: its start and
// its end both stay readable, "JUNO_A15..STRINGS"); else as th_label_set
void th_label_fit (lv_obj_t *l, const char *text);
// the same into a buffer for a painter: t cut in the middle to w pixels of f (out holds cap bytes)
void th_fit_middle (char *out, int cap, const lv_font_t *f, const char *t, int w);

enum { TH_METER_H = 0, TH_METER_V = 1, TH_METER_KNOB = 2 };
lv_obj_t *th_meter (lv_obj_t *parent, int x, int y, int w, int h, int max, int kind, uint32_t rgb);
void th_meter_set (lv_obj_t *m, int v);				// -1: not known
void th_meter_label (lv_obj_t *m, const char *caption);		// knobs: the caption under the ring
// Touch editing, the "drag" feel: th_meter_edit makes a meter take a finger, th_meter_edit_set (every refresh) says whether it
// may now (0: display only, not clickable), the BASE value a drag starts from (the knob as set, not as it plays: an LFO or a
// lock does not move the start) and the default a double tap sends. A knob changes by a vertical drag (up: more, five touch
// sizes the whole range), a bar by a drag along it (its length the whole range), always relative to where the finger landed
// (nothing happens inside a dead zone of a quarter touch size, so a tap is a tap; then the value goes on from there). cb
// (m, v, ud): v 0..max, at most every 30 ms while dragging, v | TH_RELEASED once at the lift (always: the final value),
// v | TH_DEFAULT on the double tap (v the default; a def below 0: no default, a double tap is two taps). The meter shows its own value while the finger is down and for 300 ms
// after; th_meter_get / th_meter_editing tell a page which (its value texts follow). The object is the touch target: th_meter_inset
// draws the bar smaller inside it (left, top, right, bottom), so a thin bar has a hand-sized place to take.
#define TH_DEFAULT 0x20000
void th_meter_edit (lv_obj_t *m, th_cb_t cb, void *ud);
void th_meter_edit_set (lv_obj_t *m, int base, int def, int on);
void th_meter_inset (lv_obj_t *m, int l, int t, int r, int b);
// For a value the engine only learns by a round trip (VOICE: the Rytm's own): after the lift the meter keeps the dragged value for
// up to ms, and gives way as soon as the engine's value (th_meter_set) moves; th_meter_edit_cancel drops a drag and the held value
// (another track, the write refused: the engine's value is shown again)
void th_meter_edit_hold (lv_obj_t *m, int ms);
void th_meter_edit_cancel (lv_obj_t *m);
int th_meter_span (lv_obj_t *m);				// the pixels of drag a whole range takes (the tests' rule)
int th_meter_get (lv_obj_t *m);					// the value drawn now (the dragged one while editing)
int th_meter_editing (lv_obj_t *m);				// the finger is down, or lifted a moment ago
void th_meter_value (lv_obj_t *m, const char *value);
void th_meter_value_local (lv_obj_t *m, const char *value);	// an edited meter's own text (the finger's): the page's comes back when the hold ends
const char *th_meter_value_get (lv_obj_t *m);			// the text drawn (tests)		// knobs: the value under the caption ("" none; "---" not known)

// a slider: cb (obj, value, ud) while dragging, and once more on release with idx | TH_RELEASED
#define TH_RELEASED 0x10000
lv_obj_t *th_slider (lv_obj_t *parent, int x, int y, int w, int h, int min, int max, th_cb_t cb, void *ud);
void th_slider_set (lv_obj_t *s, int v);
int th_slider_get (lv_obj_t *s);
int th_slider_dragging (lv_obj_t *s);
// a double tap (two short presses at one place within 450 ms) puts the slider at def: cb (obj, def | TH_RELEASED | TH_DEFAULT)
// once, on the second lift (its first tap did as any tap does); the slider's own drag stays absolute
void th_slider_default (lv_obj_t *s, int def);

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
