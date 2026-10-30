// pi/ui/sidecar_theme.c - see sidecar_theme.h. The two themes' tables, the dot fonts as LVGL fonts, the widgets and their
// painters. Every widget is a plain lv_obj drawn in its DRAW_MAIN by the current theme's painter: no LVGL theme, no
// styles per state, so a theme switch is a table pointer and a rebuild (sidecar_ui_style).
#include "sidecar_theme.h"
#include "src/misc/lv_area_private.h"	// lv_area_intersect
#include "src/misc/lv_text_private.h"	// lv_text_get_width
#include <string.h>

// ---- the dot fonts as LVGL fonts (A8 glyphs of 0 / 255: no anti-aliasing, so the dot grid stays crisp)

typedef struct { const dotfont_t *f; uint8_t s, bold; } dotlv_t;

static bool dot_dsc (const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
	(void) next;
	const dotlv_t *d = (const dotlv_t *) font->dsc;
	if (cp < 0x20) return false;
	const dotglyph_t *gl = dotfont_find (d->f, cp);
	if (!gl) gl = dotfont_find (d->f, '?');
	int w = gl->w + d->bold;
	g->adv_w = (uint16_t) ((w + 1) * d->s);
	g->box_w = (uint16_t) (w * d->s);
	g->box_h = (uint16_t) (d->f->h * d->s);
	g->ofs_x = 0;
	g->ofs_y = 0;
	g->stride = 0;
	g->format = LV_FONT_GLYPH_FORMAT_A8;			// dot_bitmap writes A8 (0 / 255)
	g->is_placeholder = 0;
	g->gid.src = gl;
	g->entry = NULL;
	if (cp == ' ') g->box_w = 0;				// nothing to draw
	return true;
}

static const void *dot_bitmap (lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf)
{
	const dotlv_t *d = (const dotlv_t *) g->resolved_font->dsc;
	const dotglyph_t *gl = (const dotglyph_t *) g->gid.src;
	if (!gl || !buf) return NULL;
	uint8_t *out = buf->data;
	uint32_t stride = buf->header.stride;
	int s = d->s, w = gl->w + d->bold;
	for (int r = 0; r < d->f->h; r++)
	{
		unsigned bits = gl->row[r];
		if (d->bold) bits |= bits << 1;			// each lit column twice: one column wider
		for (int j = 0; j < s; j++)
		{
			uint8_t *o = out + (uint32_t) (r * s + j) * stride;
			for (int k = 0; k < w; k++)
			{
				uint8_t v = (bits >> (w - 1 - k) & 1) ? 0xFF : 0x00;
				for (int i = 0; i < s; i++) o[k * s + i] = v;
			}
		}
	}
	return buf;						// LVGL 9.4 takes the draw buffer itself
}

// line height: the face's rows and one dot above and below; the base line one dot up. On the 2x2-pixel grid the faces
// keep the 3x3 grid's physical sizes: micro 5x7 (14 px, was 3x5 at 15 px), body and title 5x9 (18 px, was 4x6 at 18 px
// and 5x7 at 21 px), big 5x9 bold at 2x (36 px)
static const dotlv_t kMicroDsc = {&dotfont_5x7, 1, 0}, kTextDsc = {&dotfont_5x9, 1, 0}, kBigDsc = {&dotfont_5x9, 2, 1};
static const lv_font_t kDotMicro = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 9, .base_line = 1, .dsc = &kMicroDsc};
static const lv_font_t kDotText = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 11, .base_line = 1, .dsc = &kTextDsc};
static const lv_font_t kDotBig = {.get_glyph_dsc = dot_dsc, .get_glyph_bitmap = dot_bitmap, .line_height = 20, .base_line = 2, .dsc = &kBigDsc};

// ---- the themes

#define OLED_TEAL	0x5ABCE6	// the FUNC key #2B5A6E at full brightness (x2.1, the hue kept)
#define OLED_TEAL_HI	0x9CD7F0	// its light tint: selection, attention

static const sidecar_theme_t kOled = {
	.id = THEME_OLED, .name = "OLED", .dot = SIDECAR_DOT, .w = 400, .h = 240, .bw = 400, .bh = 240,
	// the rows outside the scale an even dark grey, not DOT_SHADE's checkerboard (it read as noise behind the notes)
	// the greys two steps lighter than the first cut (the 4x6 body face is thinner, and the panel loses the dark greys
	// at a slight angle: the grid and faint text must still read off-axis)
	.bg = 0x000000, .ink = 0xF0F0F0, .dim = 0xBEBEBE, .faint = 0x7C7C7C, .sub = 0xBEBEBE,
	.win = 0x000000, .win_edge = DOT_DOTTED, .win_ink = 0xF0F0F0, .win_dim = 0xBEBEBE,
	.title_bg = DOT_DITHER, .title_ink = 0x000000, .sel_bg = 0xE0E0E0, .sel_ink = 0x000000,
	// the Rytm MKII's panel (rytm-emu src/gui/hw.rs, CLASSIC's HW_FUNC and sub-label print), lit as dots: the FUNC key's
	// teal #2B5A6E scaled to full brightness is the one accent, a light tint of it the selection; the panel's blue print
	// #2C5C9E brightened is the secondary (single notes, FX 1). No red, no amber: states differ by brightness, dither, outline
	.accent = OLED_TEAL, .accent_hi = OLED_TEAL_HI,
	.ok = OLED_TEAL, .bad = OLED_TEAL_HI, .warn = OLED_TEAL_HI,
	.play = 0xFFFFFF, .lock = 0xD8D8D8,
	.row_in = 0x000000, .row_out = 0x262626, .row_root = 0x1C3A48, .line_step = 0x383838, .line_beat = 0x808080,
	.line_page = 0xC4C4C4, .key_white = 0xA0A0A0, .key_black = 0x505050, .muted = 0x909090,
	// 5x9 the body and titles, 5x7 the micro face (the sizes of the 3x3 grid's 4x6 and 3x5)
	.f_micro = &kDotMicro, .f_small = &kDotText, .f_body = &kDotText, .f_big = &kDotBig, .f_title = &kDotText,
	.radius = 0, .gap = 3, .touch = 21,
};

static const sidecar_theme_t kClassic = {
	.id = THEME_CLASSIC, .name = "CLASSIC", .dot = 1, .w = 800, .h = 480, .bw = 800, .bh = 480,
	// the Analog Rytm MKII's panel (rytm-emu src/gui/hw.rs): light grey, dark rubber keys, dark print, blue sub labels
	.bg = 0xB3B5B8, .ink = 0x25272A, .dim = 0x55585D, .faint = 0x8C8F94, .sub = 0x2C5C9E,
	.win = 0x08090A, .win_edge = 0x3A3C40, .win_ink = 0xE8E8E8, .win_dim = 0x8A8E94,
	.title_bg = 0x161618, .title_ink = 0xF0F0F0, .sel_bg = 0x2DBD5C, .sel_ink = 0x101010,
	.accent = 0x2DBD5C, .accent_hi = 0x8BE3A6,
	.ok = 0x2DBD5C, .bad = 0xFF5050, .warn = 0xE0A040,
	.play = 0xF0F0F0, .lock = 0xE0A040,
	.row_in = 0x1A1A1A, .row_out = 0x121212, .row_root = 0x14301E, .line_step = 0x222222, .line_beat = 0x2E2E2E,
	.line_page = 0x4A4A4A, .key_white = 0x2A2A2A, .key_black = 0x161616, .muted = 0x505050,
	.f_micro = &lv_font_montserrat_12, .f_small = &lv_font_montserrat_14, .f_body = &lv_font_montserrat_16,
	.f_big = &lv_font_montserrat_24, .f_title = &lv_font_montserrat_20,
	.radius = 5, .gap = 4, .touch = 48,
};

// the hardware's own colours (CLASSIC painters)
#define HW_KEY		0x2B2D30
#define HW_KEY_INK	0xE8E8E8
#define HW_KEY_DIM	0x6E7176
#define HW_EDGE		0x141517
#define HW_SHADOW	0x7E8084
#define HW_FUNC		0x2B5A6E
#define HW_PAD		0xE4E5E6
#define HW_KNOB		0x18191B
#define HW_LED_OFF	0x4C4E52
#define HW_GROUP	0x8A8C90

const sidecar_theme_t *g_Theme = &kOled;
th_fit_fn th_fit_report;
static sidecar_theme_t s_Cur;				// the table now: the theme's, sized for the screen
static int s_bWide;

const sidecar_theme_t *sidecar_theme_get (int id) { return id == THEME_CLASSIC ? &kClassic : &kOled; }
void sidecar_theme_screen (int pw, int ph) { s_bWide = pw >= 1024 && ph >= 600; }
int sidecar_theme_wide (void) { return s_bWide; }
void sidecar_theme_set (int id)
{
	s_Cur = *sidecar_theme_get (id);
	if (s_bWide)						// 1024x600: the dots / pixels it has
	{
		s_Cur.w = 1024 / s_Cur.dot;
		s_Cur.h = 600 / s_Cur.dot;
	}
	s_Cur.dx = s_Cur.w - s_Cur.bw;
	s_Cur.dy = s_Cur.h - s_Cur.bh;
	g_Theme = &s_Cur;
}

lv_color_t th_c (uint32_t rgb) { return lv_color_hex (rgb); }
// an LED colour: CLASSIC the palette's; OLED has no LED hues - white (the palette's white ramp: playing, REC's blink),
// the panel teal (green: connected, OK), its light tint (anything else)
uint32_t th_led (int c)
{
	if (TH->id != THEME_OLED) return sidecar_led_rgb (c);
	if (c >= LED_WHITE) return 0xFFFFFF;
	if (c >= LED_GREEN && c < LED_BLUE) return TH->accent;
	return TH->accent_hi;
}
uint32_t th_oled_secondary (int vel)	// the panel's blue print, brighter the louder (1..127)
{
	return th_mix (0x162E50, 0x4691FA, 60 + (vel < 1 ? 1 : vel > 127 ? 127 : vel) * 196 / 127);
}
uint32_t th_oled_accent (int vel)	// the FUNC teal, brighter the louder
{
	return th_mix (0x163040, OLED_TEAL, 60 + (vel < 1 ? 1 : vel > 127 ? 127 : vel) * 196 / 127);
}

uint32_t th_mix (uint32_t a, uint32_t b, int t)
{
	uint32_t o = 0;
	for (int s = 16; s >= 0; s -= 8)
	{
		int x = (int) (a >> s & 255), y = (int) (b >> s & 255);
		o |= (uint32_t) (x + (y - x) * t / 256) << s;
	}
	return o;
}

static int oled (void) { return TH->id == THEME_OLED; }

// ---- painters

void th_fill (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, lv_opa_t opa, int r)
{
	lv_area_t a = {c->x1 + x1, c->y1 + y1, c->x1 + x2, c->y1 + y2}, k;
	if (x2 < x1 || y2 < y1 || !lv_area_intersect (&k, &a, &l->_clip_area)) return;
	lv_draw_fill_dsc_t d;
	lv_draw_fill_dsc_init (&d);
	d.color = lv_color_hex (rgb);
	d.opa = opa;
	d.radius = r;
	lv_draw_fill (l, &d, &a);
}

void th_frame (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb, int w, int r)
{
	if (oled () || r == 0)					// square: four fills (crisp, and DOT_* colours stay exact)
	{
		th_fill (l, c, x1, y1, x2, y1 + w - 1, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x1, y2 - w + 1, x2, y2, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x1, y1 + w, x1 + w - 1, y2 - w, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x2 - w + 1, y1 + w, x2, y2 - w, rgb, LV_OPA_COVER, 0);
		return;
	}
	lv_area_t a = {c->x1 + x1, c->y1 + y1, c->x1 + x2, c->y1 + y2}, k;
	if (!lv_area_intersect (&k, &a, &l->_clip_area)) return;
	lv_draw_border_dsc_t d;
	lv_draw_border_dsc_init (&d);
	d.color = lv_color_hex (rgb);
	d.width = w;
	d.radius = r;
	d.opa = LV_OPA_COVER;
	lv_draw_border (l, &d, &a);
}

void th_text (lv_layer_t *l, const lv_area_t *c, int x, int y, int w, const lv_font_t *f, uint32_t rgb, lv_text_align_t al,
	      const char *t)
{
	lv_area_t a = {c->x1 + x, c->y1 + y, c->x1 + x + w - 1, c->y1 + y + f->line_height - 1}, k;
	if (!t || !t[0] || !lv_area_intersect (&k, &a, &l->_clip_area)) return;
	if (th_fit_report)
	{
		int need = th_text_width (f, t), ow = lv_area_get_width (c);
		int x0 = al == LV_TEXT_ALIGN_CENTER ? x + (w - need) / 2 : al == LV_TEXT_ALIGN_RIGHT ? x + w - need : x;
		if (need > w) th_fit_report (t, need, w);
		else if (x0 < 0 || x0 + need > ow) th_fit_report (t, need, x0 < 0 ? need + x0 : ow - x0);	// past its object
	}
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init (&d);
	d.text = t;
	d.font = f;
	d.color = lv_color_hex (rgb);
	d.align = al;
	d.text_local = 1;
	d.flag = LV_TEXT_FLAG_EXPAND;
	lv_draw_label (l, &d, &a);
}

int th_text_width (const lv_font_t *f, const char *t)
{
	lv_text_attributes_t a;
	lv_text_attributes_init (&a);
	return lv_text_get_width (t, (uint32_t) strlen (t), f, &a);
}

// a disc of dots (no anti-aliasing): rows of fills
static void disc (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, uint32_t rgb)
{
	for (int dy = -r; dy <= r; dy++)
	{
		int dx = 0;
		while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
		th_fill (l, c, cx - dx, cy + dy, cx + dx, cy + dy, rgb, LV_OPA_COVER, 0);
	}
}

static void circle (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, uint32_t rgb, lv_opa_t opa)
{
	th_fill (l, c, cx - r, cy - r, cx + r, cy + r, rgb, opa, LV_RADIUS_CIRCLE);
}

// sin / cos x 1024 of 0..359 degrees, from a quarter table
static int isin (int deg)
{
	static const short q[91] = {0, 18, 36, 54, 71, 89, 107, 125, 143, 160, 178, 195, 213, 230, 248, 265, 282, 299, 316, 333,
		350, 367, 384, 400, 416, 433, 449, 465, 481, 496, 512, 527, 543, 558, 573, 587, 602, 616, 630, 644, 658, 672, 685,
		698, 711, 724, 737, 749, 761, 773, 784, 796, 807, 818, 828, 839, 849, 859, 868, 878, 887, 896, 904, 912, 920, 928,
		935, 943, 949, 956, 962, 968, 974, 979, 984, 989, 994, 998, 1002, 1005, 1008, 1011, 1014, 1016, 1018, 1020, 1022,
		1023, 1023, 1024, 1024};
	deg = ((deg % 360) + 360) % 360;
	if (deg <= 90) return q[deg];
	if (deg <= 180) return q[180 - deg];
	if (deg <= 270) return -q[deg - 180];
	return -q[360 - deg];
}
static int icos (int deg) { return isin (deg + 90); }

void th_paint_knob (lv_layer_t *l, const lv_area_t *a, int v, int max, uint32_t rgb)
{
	lv_area_t c = *a;
	int w = lv_area_get_width (a), h = lv_area_get_height (a);
	int r = (w < h ? w : h) / 2 - 1, cx = w / 2, cy = h / 2;
	if (max <= 0) max = 1;
	int known = v >= 0;
	if (v < 0) v = 0;
	if (v > max) v = max;
	int ang = 225 - 270 * v / max;				// 225 (min, lower left) .. -45 (max, lower right), screen y down
	if (oled ())
	{
		const int n = r >= 14 ? 19 : 13;			// the finer grid: more dots on a big ring
		int lit = known ? (v * (n - 1) + max / 2) / max : -1;
		for (int i = 0; i < n; i++)		// the scale: a ring of dots, lit up to the value
		{
			int d = 225 - 270 * i / (n - 1);
			int x = cx + (icos (d) * r + (icos (d) > 0 ? 512 : -512)) / 1024, y = cy - (isin (d) * r + (isin (d) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, i <= lit ? rgb : TH->faint, LV_OPA_COVER, 0);
		}
		// UX review T7: a hollow cap (an outline of dots, not the page's brightest mass) and the pointer as a line of
		// ink dots from the centre to the cap's edge; a value not known yet: the outline faint, no pointer
		int rb = r - (r >= 14 ? 4 : 3), nd = rb >= 10 ? 28 : 20;
		for (int i = 0; i < nd; i++)
		{
			int d = 360 * i / nd;
			int x = cx + (icos (d) * rb + (icos (d) > 0 ? 512 : -512)) / 1024, y = cy - (isin (d) * rb + (isin (d) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, known ? 0x8C8C8C : 0x5A5A5A, LV_OPA_COVER, 0);
		}
		if (!known) return;
		for (int k = 0; k <= rb - 1; k++)
		{
			int x = cx + (icos (ang) * k + (icos (ang) > 0 ? 512 : -512)) / 1024;
			int y = cy - (isin (ang) * k + (isin (ang) > 0 ? 512 : -512)) / 1024;
			th_fill (l, &c, x, y, x, y, TH->ink, LV_OPA_COVER, 0);
		}
		return;
	}
	// CLASSIC: an LED ring around a dark knob cap lit from the top left (rytm-emu draw_knob)
	const int n = 15;
	int lit = known ? (v * (n - 1) + max / 2) / max : -1;
	int rr = r - 3;
	for (int i = 0; i < n; i++)
	{
		int d = 225 - 270 * i / (n - 1);
		int x = cx + icos (d) * rr / 1024, y = cy - isin (d) * rr / 1024;
		if (i <= lit)
		{
			circle (l, &c, x, y, 4, th_mix (TH->win, rgb, 90), LV_OPA_COVER);	// knobs sit in display windows
			circle (l, &c, x, y, 2, rgb, LV_OPA_COVER);
		}
		else circle (l, &c, x, y, 2, HW_LED_OFF, LV_OPA_COVER);
	}
	int kr = rr - 9;
	circle (l, &c, cx + 2, cy + 3, kr, 0x000000, LV_OPA_COVER);
	circle (l, &c, cx, cy, kr, th_mix (HW_KNOB, 0, 70), LV_OPA_COVER);
	for (int k = 1; k <= 3; k++)
		circle (l, &c, cx - kr / 4 - k / 2, cy - kr / 4 - k / 2, kr * (6 - k) / 8, th_mix (HW_KNOB, 0xFFFFFF, 10 * k), LV_OPA_COVER);
	if (!known) return;					// (not known yet: no pointer)
	int px = cx + icos (ang) * (kr - 4) / 1024, py = cy - isin (ang) * (kr - 4) / 1024;
	circle (l, &c, px, py, 2, 0xF0F0F0, LV_OPA_COVER);
}

static int font_line (const lv_font_t *f) { return f->line_height; }

// the navigation column's icons (OLED), 15 x 11 dots, drawn in the key's ink; the key's text picks one and stays as a
// micro label under it
#define ICON_W	15
#define ICON_H	11
static const struct { const char *name; const char *row[ICON_H]; } kIcon[] = {
	{"HOME", {	".......#.......",		// a house, its door
			"......#.#......",
			".....#...#.....",
			"....#.....#....",
			"...#.......#...",
			"..###########..",
			"..#.........#..",
			"..#..###....#..",
			"..#..#.#....#..",
			"..#..#.#....#..",
			"..####.######.."}},
	{"SEQ", {	"##.#...........",		// a piano roll: keys, notes
			"#..............",
			"#....######....",
			"#..............",
			"##.#.......###.",
			"#..............",
			"#..####........",
			"#..............",
			"##.#....#####..",
			"#..............",
			"###############"}},
	{"PAT", {	"###.###.###.###",		// trig keys, some lit
			"###.#.#.###.#.#",
			"###.###.###.###",
			"...............",
			"###.###.###.###",
			"#.#.###.#.#.#.#",
			"###.###.###.###",
			"...............",
			"###.###.###.###",
			"#.#.#.#.###.###",
			"###.###.###.###"}},
	{"SET", {	"...###.........",		// three sliders
			"###############",
			"...###.........",
			"...............",
			".........###...",
			"###############",
			".........###...",
			"...............",
			".....###.......",
			"###############",
			".....###......."}},
	{"VOICE", {	".....#####.....",		// a knob, its pointer
			"...##.....##...",
			"..#.........#..",
			".#.....#.....#.",
			".#.....#.....#.",
			".#.....#.....#.",
			".#...........#.",
			"..#.........#..",
			"...##.....##...",
			".....#####.....",
			"..............."}},
	{"FX", {	"#######........",		// a filter's curve
			".......##......",
			".........#.....",
			"..........#....",
			"...........#...",
			"...........#...",
			"............#..",
			"............#..",
			".............#.",
			".............#.",
			"..............."}},
	{"SONG", {	"###############",		// a timeline: pattern blocks over a ruler
			"#.....#....#..#",
			"#.....#....#..#",
			"###############",
			"...............",
			"..#####........",
			"...............",
			"........######.",
			"...............",
			"#..#..#..#..#..",
			"###############"}},
	{"REC", {	"...............",		// the record dot
			"......###......",
			"....#######....",
			"...#########...",
			"..###########..",
			"..###########..",
			"..###########..",
			"...#########...",
			"....#######....",
			"......###......",
			"..............."}},
	{"BACK", {	".....#.........",		// an arrow round and back
			"....##.........",
			"...##########..",
			"..###########..",
			"...##.......##.",
			"....#........#.",
			".............#.",
			".............#.",
			".............#.",
			"....##########.",
			"..............."}},
};

static int nav_icon (lv_layer_t *l, const lv_area_t *c, const char *name, int cx, int y, uint32_t ink)
{
	for (unsigned i = 0; i < sizeof kIcon / sizeof kIcon[0]; i++)
	{
		if (strcmp (kIcon[i].name, name)) continue;
		for (int r = 0; r < ICON_H; r++)
			for (int k = 0; k < ICON_W; k++)
				if (kIcon[i].row[r][k] == '#') th_fill (l, c, cx - ICON_W / 2 + k, y + r, cx - ICON_W / 2 + k, y + r, ink, LV_OPA_COVER, 0);
		return 1;
	}
	return 0;
}

void th_paint_key (lv_layer_t *l, const lv_area_t *a, const char *text, const char *sub, const lv_font_t *f, int kind,
		   int checked, int pressed, int disabled, int led)
{
	lv_area_t c = *a;
	int w = lv_area_get_width (a), h = lv_area_get_height (a), k = kind & TH_KEY_KIND;
	int left = kind & TH_KEY_LEFT;
	if (!f) f = kind & TH_KEY_SMALL ? TH->f_micro : k == TH_KEY_NAV ? TH->f_title : TH->f_body;
	int th = font_line (f) + (sub && sub[0] ? font_line (TH->f_micro) : 0);
	int ty = (h - th) / 2;
	if (oled ())
	{
		uint32_t ink = TH->ink, edge = 0xC8C8C8;
		if (k == TH_KEY_DANGER) { ink = TH->bad; edge = TH->bad; }
		if (k == TH_KEY_NAV) edge = DOT_DOTTED;
		if (disabled) { ink = TH->faint; edge = DOT_DOTTED; }
		if (pressed && !disabled)
		{
			th_fill (l, &c, 0, 0, w - 1, h - 1, DOT_DITHER, LV_OPA_COVER, 0);
			ink = 0x000000;
		}
		else if ((checked || (kind & TH_KEY_FILL)) && !disabled)
		{
			th_fill (l, &c, 0, 0, w - 1, h - 1, k == TH_KEY_DANGER ? TH->bad : TH->sel_bg, LV_OPA_COVER, 0);
			ink = TH->sel_ink;
		}
		else th_fill (l, &c, 0, 0, w - 1, h - 1, 0x000000, LV_OPA_COVER, 0);
		if (k != TH_KEY_PAD || !checked) th_frame (l, &c, 0, 0, w - 1, h - 1, edge, 1, 0);
		if (k == TH_KEY_ACCENT && !checked && !pressed)	// the main action: a double frame
			th_frame (l, &c, 3, 3, w - 4, h - 4, DOT_DOTTED, 1, 0);
		if (led > 0)
		{
			uint32_t lc = th_led (led);
			if (k == TH_KEY_TRIG || k == TH_KEY_NAV) th_fill (l, &c, 4, h - 4, w - 5, h - 3, lc, LV_OPA_COVER, 0);	// under the text
			else th_fill (l, &c, w - 7, 2, w - 3, 4, lc, LV_OPA_COVER, 0);			// a corner LED
		}
		int tx = left ? 6 : 0, tw = left ? w - 8 : w;
		if (k == TH_KEY_NAV && text)			// an icon, its name small under it
		{
			int iy = (h - ICON_H - 3 - 7) / 2, dx = sub && sub[0] ? 4 : 0;
			if (nav_icon (l, &c, text, w / 2 + dx, iy, ink))	// (clear of the corner's trig)
			{
				th_text (l, &c, 2 * dx + 4, iy + ICON_H + 2, w - 2 * dx - 4, TH->f_micro, ink, LV_TEXT_ALIGN_CENTER, text);	// under the icon, 2 px right for balance (user)
				if (sub && sub[0])			// UX review N1: its UP + T trig in the corner
					th_text (l, &c, 2, 1, w - 4, TH->f_micro, checked || pressed ? ink : TH->faint, LV_TEXT_ALIGN_LEFT, sub);
				return;
			}
		}
		th_text (l, &c, tx, ty, tw, f, ink, left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0])
			th_text (l, &c, tx, ty + font_line (f), tw, TH->f_micro, checked || pressed ? ink : TH->dim,
				 left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, sub);
		return;
	}
	// CLASSIC: the Rytm MKII's keys (rytm-emu draw_key_mk2): a drop shadow, the rubber face with a highlight line on top,
	// a dark edge, or the LED's colour around it and its glow; the print LED-tinted when lit
	uint32_t lc = led > 0 ? th_led (led) : checked ? (k == TH_KEY_DANGER ? TH->bad : TH->accent) : 0;
	int lit = lc != 0 && !disabled;
	if (k == TH_KEY_PAD)
	{
		uint32_t face = HW_PAD;
		if (lit)
		{
			for (int i = 6; i >= 2; i -= 2) th_fill (l, &c, -i, -i, w - 1 + i, h - 1 + i, th_mix (TH->bg, lc, 40 + (6 - i) * 18), LV_OPA_COVER, 8 + i);
			face = th_mix (face, lc, 120);
		}
		else th_fill (l, &c, 1, 3, w, h + 2, HW_SHADOW, LV_OPA_COVER, 7);
		if (pressed) face = th_mix (face, 0, 40);
		th_fill (l, &c, 0, 0, w - 1, h - 1, face, LV_OPA_COVER, 7);
		th_frame (l, &c, 0, 0, w - 1, h - 1, th_mix (face, 0, 50), 1, 7);
		th_text (l, &c, left ? 12 : 0, ty, left ? w - 16 : w, f, disabled ? HW_KEY_DIM : 0x25272A,
			 left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, text);
		if (sub && sub[0])
			th_text (l, &c, left ? 12 : 0, ty + font_line (f), left ? w - 16 : w, TH->f_micro, 0x55585D,
				 left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, sub);
		return;
	}
	int big = k == TH_KEY_TRIG || h >= 56, r = big ? 6 : 5;
	uint32_t face = k == TH_KEY_FUNC ? HW_FUNC : HW_KEY;
	if ((kind & TH_KEY_FILL) && !disabled) face = th_mix (face, k == TH_KEY_DANGER ? TH->bad : TH->accent, 140);	// filled (T16)
	if (pressed && !disabled) face = th_mix (face, 0xFFFFFF, 60);
	th_fill (l, &c, 1, 2, w, h + 1, HW_SHADOW, LV_OPA_COVER, r);
	if (lit) th_frame (l, &c, -3, -3, w + 2, h + 2, th_mix (TH->bg, lc, 90), 2, r + 3);
	th_fill (l, &c, 0, 0, w - 1, h - 1, face, LV_OPA_COVER, r);
	th_fill (l, &c, r, 1, w - 1 - r, 1, th_mix (face, 0xFFFFFF, 30), LV_OPA_COVER, 0);
	if (lit) th_frame (l, &c, 0, 0, w - 1, h - 1, lc, 2, r);
	else th_frame (l, &c, 0, 0, w - 1, h - 1, HW_EDGE, 1, r);
	uint32_t ink = disabled ? th_mix (HW_KEY_DIM, face, 100) : lit ? th_mix (HW_KEY_DIM, th_mix (lc, 0xFFFFFF, 60), 230) : HW_KEY_INK;
	if (k == TH_KEY_DANGER && !lit && !disabled) ink = (kind & TH_KEY_FILL) ? 0xFFFFFF : 0xFF8080;
	th_text (l, &c, left ? 10 : 0, ty, left ? w - 14 : w, f, ink, left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, text);
	if (k == TH_KEY_TRIG && text && text[0])		// the trig keys' underline
	{
		int tw = th_text_width (f, text);
		th_fill (l, &c, (w - tw) / 2, ty + font_line (f) - 2, (w + tw) / 2, ty + font_line (f) - 1, ink, LV_OPA_COVER, 0);
	}
	if (sub && sub[0])
		th_text (l, &c, left ? 10 : 0, ty + font_line (f), left ? w - 14 : w, TH->f_micro, disabled ? ink : 0x8E9196,
			 left ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, sub);
}

// ---- widgets

typedef struct
{
	char		text[24], sub[24];
	int16_t		kind, idx;
	int16_t		led, row;
	uint8_t		checkable;
	const lv_font_t	*font;
	th_cb_t		cb;
	void		*ud;
} key_t;

typedef struct
{
	th_cb_t		cb;
	void		*ud;
	int16_t		n, sel, kind, gap, x, y, w, h;
} seg_t;

static void free_ud (lv_event_t *e)
{
	void *p = lv_obj_get_user_data (lv_event_get_target_obj (e));
	if (p) lv_free (p);
}

static void copy (char *d, const char *s, int cap)
{
	int i = 0;
	for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

lv_obj_t *th_box (lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create (parent);
	lv_obj_remove_style_all (o);
	lv_obj_set_pos (o, x, y);
	lv_obj_set_size (o, w, h);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_CLICKABLE);
	return o;
}

static void key_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	th_paint_key (lv_event_get_layer (e), &a, k->text, k->sub, k->font, k->kind, lv_obj_has_state (o, LV_STATE_CHECKED),
		      lv_obj_has_state (o, LV_STATE_PRESSED), lv_obj_has_state (o, LV_STATE_DISABLED), k->led);
}

static void key_ev (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_event_code_t code = lv_event_get_code (e);
	if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) { lv_event_set_ext_draw_size (e, 6); return; }
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		lv_area_t a;
		lv_obj_get_coords (o, &a);
		lv_area_increase (&a, 6, 6);			// the CLASSIC glow and shadow outside the key
		lv_obj_invalidate_area (o, &a);
		return;
	}
	if (code != LV_EVENT_CLICKED || lv_obj_has_state (o, LV_STATE_DISABLED)) return;
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	lv_obj_t *p = lv_obj_get_parent (o);
	if (k->idx >= 0)					// a seg's item
	{
		seg_t *s = (seg_t *) lv_obj_get_user_data (p);
		if (k->checkable) th_seg_set (p, k->idx);
		if (s->cb) s->cb (p, k->idx, s->ud);
		return;
	}
	if (k->cb) k->cb (o, 0, k->ud);
}

static void inval_key (lv_obj_t *o)
{
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	lv_area_increase (&a, 6, 6);
	lv_obj_invalidate_area (o, &a);
}

static lv_obj_t *key_new (lv_obj_t *parent, int x, int y, int w, int h, const char *text, int kind, int idx)
{
	lv_obj_t *o = lv_obj_create (parent);
	lv_obj_remove_style_all (o);
	lv_obj_set_pos (o, x, y);
	lv_obj_set_size (o, w, h);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	key_t *k = (key_t *) lv_malloc_zeroed (sizeof (key_t));
	copy (k->text, text, sizeof k->text);
	k->kind = (int16_t) kind;
	k->idx = (int16_t) idx;
	k->checkable = 1;
	lv_obj_set_user_data (o, k);
	lv_obj_add_event_cb (o, key_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, key_ev, LV_EVENT_ALL, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

lv_obj_t *th_key (lv_obj_t *parent, int x, int y, int w, int h, const char *text, int kind, th_cb_t cb, void *ud)
{
	lv_obj_t *o = key_new (parent, x, y, w, h, text, kind, -1);
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	k->cb = cb;
	k->ud = ud;
	return o;
}

void th_key_text (lv_obj_t *o, const char *text)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (strncmp (k->text, text, sizeof k->text - 1) == 0) return;
	copy (k->text, text, sizeof k->text);
	inval_key (o);
}

void th_key_sub (lv_obj_t *o, const char *sub)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (strncmp (k->sub, sub, sizeof k->sub - 1) == 0) return;
	copy (k->sub, sub, sizeof k->sub);
	inval_key (o);
}

void th_key_led (lv_obj_t *o, int c)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (k->led == c) return;
	k->led = (int16_t) c;
	inval_key (o);
}

void th_key_kind (lv_obj_t *o, int kind)
{
	key_t *k = (key_t *) lv_obj_get_user_data (o);
	if (k->kind == kind) return;
	k->kind = (int16_t) kind;
	inval_key (o);
}

void th_key_font (lv_obj_t *o, const lv_font_t *f)
{
	((key_t *) lv_obj_get_user_data (o))->font = f;
	inval_key (o);
}

void th_seg_font (lv_obj_t *seg, const lv_font_t *f)
{
	for (int i = 0; i < th_seg_count (seg); i++) th_key_font (th_seg_item (seg, i), f);
}

void th_key_check (lv_obj_t *o, int on)
{
	if (!!lv_obj_has_state (o, LV_STATE_CHECKED) == !!on) return;
	if (on) lv_obj_add_state (o, LV_STATE_CHECKED); else lv_obj_remove_state (o, LV_STATE_CHECKED);
	inval_key (o);
}

void th_key_enable (lv_obj_t *o, int on)
{
	if (!lv_obj_has_state (o, LV_STATE_DISABLED) == !!on) return;
	if (on) lv_obj_remove_state (o, LV_STATE_DISABLED); else lv_obj_add_state (o, LV_STATE_DISABLED);
	inval_key (o);
}

// the items' places: rows split by "\n", each row's items share the width
static void seg_layout (lv_obj_t *seg, const char *const *map, int create)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	int rows = 1, n = 0;
	for (int i = 0; map[i][0]; i++) if (strcmp (map[i], "\n") == 0) rows++;
	int rh = (s->h - (rows - 1) * s->gap) / rows, row = 0, i = 0;
	while (map[i][0])
	{
		int cnt = 0;
		while (map[i + cnt][0] && strcmp (map[i + cnt], "\n")) cnt++;
		int y = row * (rh + s->gap);
		for (int j = 0; j < cnt; j++)
		{
			int x1 = j * (s->w + s->gap) / cnt, x2 = (j + 1) * (s->w + s->gap) / cnt - s->gap;
			lv_obj_t *it = create ? 0 : th_seg_item (seg, n);
			if (!it)
			{
				it = key_new (seg, x1, y, x2 - x1, rh, map[i + j], s->kind | TH_KEY_HOLD, n);
				const lv_font_t *f = n ? ((key_t *) lv_obj_get_user_data (th_seg_item (seg, 0)))->font : 0;
				((key_t *) lv_obj_get_user_data (it))->font = f;	// a new item takes the others' font
			}
			else
			{
				lv_obj_set_pos (it, x1, y);
				lv_obj_set_size (it, x2 - x1, rh);
				th_key_text (it, map[i + j]);
				lv_obj_remove_flag (it, LV_OBJ_FLAG_HIDDEN);
			}
			((key_t *) lv_obj_get_user_data (it))->row = (int16_t) row;
			n++;
		}
		i += cnt;
		if (map[i][0]) { i++; row++; }
	}
	for (uint32_t k = (uint32_t) n; k < lv_obj_get_child_count (seg); k++) lv_obj_add_flag (lv_obj_get_child (seg, (int32_t) k), LV_OBJ_FLAG_HIDDEN);
	s->n = (int16_t) n;
	if (s->sel >= n) s->sel = -1;
}

lv_obj_t *th_seg (lv_obj_t *parent, int x, int y, int w, int h, const char *const *map, int kind, th_cb_t cb, void *ud)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	seg_t *s = (seg_t *) lv_malloc_zeroed (sizeof (seg_t));
	s->cb = cb; s->ud = ud; s->sel = -1; s->kind = (int16_t) kind; s->gap = (int16_t) TH->gap;
	s->x = (int16_t) x; s->y = (int16_t) y; s->w = (int16_t) w; s->h = (int16_t) h;
	lv_obj_set_user_data (o, s);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	seg_layout (o, map, 1);
	return o;
}

void th_seg_gap (lv_obj_t *seg, int gap)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	s->gap = (int16_t) gap;
	// the items keep their texts: rebuilt from them
	static char t[64][24];
	static const char *m[64 * 2 + 1];
	int n = s->n, k = 0;
	for (int i = 0; i < n && i < 64; i++)
	{
		lv_obj_t *it = th_seg_item (seg, i);
		copy (t[i], ((key_t *) lv_obj_get_user_data (it))->text, 24);
		if (i && ((key_t *) lv_obj_get_user_data (it))->row != ((key_t *) lv_obj_get_user_data (th_seg_item (seg, i - 1)))->row) m[k++] = "\n";
		m[k++] = t[i];
	}
	m[k] = "";
	seg_layout (seg, m, 0);
}

void th_seg_map (lv_obj_t *seg, const char *const *map)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	int old = s->n;
	seg_layout (seg, map, 0);
	for (int i = old; i < s->n; i++) th_key_check (th_seg_item (seg, i), 0);
	lv_obj_invalidate (seg);
}

lv_obj_t *th_seg_item (lv_obj_t *seg, int idx)
{
	if (idx < 0 || idx >= (int) lv_obj_get_child_count (seg)) return 0;
	return lv_obj_get_child (seg, idx);
}

int th_seg_count (lv_obj_t *seg) { return ((seg_t *) lv_obj_get_user_data (seg))->n; }
int th_seg_get (lv_obj_t *seg) { return ((seg_t *) lv_obj_get_user_data (seg))->sel; }

void th_seg_set (lv_obj_t *seg, int idx)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	if (idx >= s->n) idx = -1;
	if (idx >= 0 && !((key_t *) lv_obj_get_user_data (th_seg_item (seg, idx)))->checkable) return;
	if (s->sel == idx) return;
	if (s->sel >= 0) th_key_check (th_seg_item (seg, s->sel), 0);
	if (idx >= 0) th_key_check (th_seg_item (seg, idx), 1);
	s->sel = (int16_t) idx;
}

void th_seg_enable (lv_obj_t *seg, int idx, int on)
{
	seg_t *s = (seg_t *) lv_obj_get_user_data (seg);
	for (int i = 0; i < s->n; i++)
		if (idx < 0 || i == idx) th_key_enable (th_seg_item (seg, i), on);
}

void th_seg_checkable (lv_obj_t *seg, int idx, int on)
{
	lv_obj_t *it = th_seg_item (seg, idx);
	if (it) ((key_t *) lv_obj_get_user_data (it))->checkable = (uint8_t) on;
}

void th_seg_led (lv_obj_t *seg, int idx, int c)
{
	lv_obj_t *it = th_seg_item (seg, idx);
	if (it) th_key_led (it, c);
}

// ---- panels, title bars, labels

static void ext6 (lv_event_t *e) { lv_event_set_ext_draw_size (e, 6); }

typedef struct { char title[32]; int16_t kind; } panel_t;

static void panel_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	panel_t *p = (panel_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	if (p->kind == TH_PANEL_BARE) return;
	if (oled ())
	{
		// a dotted frame, the title in a notch of its top edge
		th_frame (l, &c, 0, 4, w - 1, h - 1, DOT_DOTTED, 1, 0);
		if (p->title[0])
		{
			int tw = th_text_width (TH->f_micro, p->title);
			th_fill (l, &c, 4, 0, 4 + tw + 5, 8, 0x000000, LV_OPA_COVER, 0);
			th_text (l, &c, 7, -1, tw + 2, TH->f_micro, TH->dim, LV_TEXT_ALIGN_LEFT, p->title);
		}
		return;
	}
	if (p->kind == TH_PANEL_WINDOW)		// the Rytm's display window: a bezel with a soft drop, black glass
	{
		th_fill (l, &c, 2, 3, w + 1, h + 2, 0x8A8C90, LV_OPA_COVER, 8);
		th_fill (l, &c, 0, 0, w - 1, h - 1, TH->win, LV_OPA_COVER, 8);
		th_frame (l, &c, 0, 0, w - 1, h - 1, TH->win_edge, 1, 8);
		if (p->title[0]) th_text (l, &c, 12, 8, w - 24, &lv_font_montserrat_12, TH->win_dim, LV_TEXT_ALIGN_LEFT, p->title);
		return;
	}
	// a printed group on the panel: a hairline frame, the title in the panel's dark print
	th_frame (l, &c, 0, 0, w - 1, h - 1, HW_GROUP, 1, 6);
	if (p->title[0]) th_text (l, &c, 12, 8, w - 24, &lv_font_montserrat_12, TH->ink, LV_TEXT_ALIGN_LEFT, p->title);
}

lv_obj_t *th_panel (lv_obj_t *parent, int x, int y, int w, int h, const char *title, int kind)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	panel_t *p = (panel_t *) lv_malloc_zeroed (sizeof (panel_t));
	copy (p->title, title, sizeof p->title);
	if (!oled ()) for (char *q = p->title; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
	p->kind = (int16_t) kind;
	lv_obj_set_user_data (o, p);
	lv_obj_add_event_cb (o, panel_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, ext6, LV_EVENT_REFR_EXT_DRAW_SIZE, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

int th_panel_top (void) { return oled () ? 12 : 30; }

typedef struct { char left[48], right[48]; int16_t alert, led; } title_t;

static void title_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	const lv_font_t *f = TH->f_title;
	int ty = (h - f->line_height) / 2;
	if (oled ())
	{
		th_fill (l, &c, 0, 0, w - 1, h - 1, t->alert ? DOT_ALERT : DOT_DITHER, LV_OPA_COVER, 0);
		th_text (l, &c, 7, ty, w - 10, f, 0x000000, LV_TEXT_ALIGN_LEFT, t->left);	// a margin: the text clear of the edge
		int rx = 16;						// the LED's notch kept: a blinking REC does not move the text
		if (th_fit_report && t->right[0])
		{
			int need = th_text_width (f, t->left) + 8 + th_text_width (TH->f_body, t->right);
			if (need > w - 7 - rx) th_fit_report (t->right, need, w - 7 - rx);
		}
		th_text (l, &c, 7, (h - TH->f_body->line_height) / 2, w - 7 - rx, TH->f_body, 0x000000, LV_TEXT_ALIGN_RIGHT, t->right);
		if (t->led > 0)					// a status LED at the right end: a dot block in a black notch
		{
			th_fill (l, &c, w - 12, 1, w - 2, h - 2, 0x000000, LV_OPA_COVER, 0);
			th_fill (l, &c, w - 11, 2, w - 3, h - 3, th_led (t->led), LV_OPA_COVER, 0);
		}
		return;
	}
	th_fill (l, &c, 0, 0, w - 1, h - 1, t->alert ? 0x8A1A1A : TH->title_bg, LV_OPA_COVER, 0);
	th_text (l, &c, 12, ty, w - 24, f, TH->title_ink, LV_TEXT_ALIGN_LEFT, t->left);
	int rx = t->led > 0 ? 34 : 12;
	if (th_fit_report && t->right[0])
	{
		int need = th_text_width (f, t->left) + 12 + th_text_width (TH->f_body, t->right);
		if (need > w - 12 - rx) th_fit_report (t->right, need, w - 12 - rx);
	}
	th_text (l, &c, 12, (h - TH->f_body->line_height) / 2, w - 12 - rx, TH->f_body, t->alert ? 0xFFFFFF : TH->win_dim, LV_TEXT_ALIGN_RIGHT, t->right);
	if (t->led > 0)
	{
		uint32_t lc = th_led (t->led);
		circle (l, &c, w - 20, h / 2, 9, th_mix (TH->title_bg, lc, 90), LV_OPA_COVER);
		circle (l, &c, w - 20, h / 2, 6, lc, LV_OPA_COVER);
	}
}

lv_obj_t *th_titlebar (lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	title_t *t = (title_t *) lv_malloc_zeroed (sizeof (title_t));
	lv_obj_set_user_data (o, t);
	lv_obj_add_event_cb (o, title_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_titlebar_led (lv_obj_t *o, int c)
{
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	if (t->led == c) return;
	t->led = (int16_t) c;
	lv_obj_invalidate (o);
}

void th_titlebar_set (lv_obj_t *o, const char *left, const char *right, int alert)
{
	title_t *t = (title_t *) lv_obj_get_user_data (o);
	if (!left) left = t->left;
	if (!right) right = "";
	if (!strcmp (t->left, left) && !strcmp (t->right, right) && t->alert == alert) return;
	copy (t->left, left, sizeof t->left);
	copy (t->right, right, sizeof t->right);
	t->alert = (int16_t) alert;
	lv_obj_invalidate (o);
}

lv_obj_t *th_label (lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t rgb, const char *text)
{
	lv_obj_t *l = lv_label_create (parent);
	lv_label_set_text (l, text);
	lv_obj_set_style_text_font (l, f, 0);
	lv_obj_set_style_text_color (l, lv_color_hex (rgb), 0);
	lv_obj_set_pos (l, x, y);
	return l;
}

void th_label_set (lv_obj_t *l, const char *t)
{
	if (strcmp (lv_label_get_text (l), t) != 0) lv_label_set_text (l, t);
}

void th_fit_middle (char *out, int cap, const lv_font_t *f, const char *t, int w)
{
	int n = (int) strlen (t);
	lv_snprintf (out, cap, "%s", t);
	if (th_text_width (f, out) <= w || n < 4) return;
	for (int keep = n - 1; keep >= 2; keep--)		// the most characters that fit, half from each end
	{
		int head = (keep + 1) / 2, tail = keep - head;
		if (head + tail + 3 > cap) continue;
		memcpy (out, t, (size_t) head);
		out[head] = '.', out[head + 1] = '.';
		memcpy (out + head + 2, t + n - tail, (size_t) tail);
		out[head + 2 + tail] = 0;
		if (th_text_width (f, out) <= w) return;
	}
}

void th_label_fit (lv_obj_t *l, const char *t)
{
	char b[160];
	int w = (int) lv_obj_get_style_width (l, 0);		// the width it was given (a plain number: label_w)
	if (w <= 0 || LV_COORD_IS_SPEC (w)) { th_label_set (l, t); return; }
	th_fit_middle (b, sizeof b, lv_obj_get_style_text_font (l, 0), t, w);
	th_label_set (l, b);
}

void th_label_color (lv_obj_t *l, uint32_t rgb)
{
	// UX review L2, the alert grammar: in the OLED theme the attention colour (TH->bad, the FUNC teal's tint) is never
	// plain text - the label is inverted, black on teal, so teal means "look here" and nothing else
	int alert = oled () && rgb == TH->bad;
	lv_color_t c = lv_color_hex (alert ? 0x000000 : rgb);
	if (!lv_color_eq (lv_obj_get_style_text_color (l, 0), c)) lv_obj_set_style_text_color (l, c, 0);
	if (alert != (lv_obj_get_style_bg_opa (l, 0) == LV_OPA_COVER && lv_color_eq (lv_obj_get_style_bg_color (l, 0), lv_color_hex (TH->bad))))
	{
		lv_obj_set_style_bg_color (l, lv_color_hex (TH->bad), 0);
		lv_obj_set_style_bg_opa (l, alert ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	}
}

// ---- meters and sliders

typedef struct { int16_t v, max, kind; uint32_t rgb; char cap[16], val[12]; } meter_t;

static void meter_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), v = m->v < 0 ? 0 : m->v > m->max ? m->max : m->v;
	if (m->kind == TH_METER_KNOB)
	{
		const lv_font_t *fv = TH->f_micro;
		int ch = m->cap[0] ? TH->f_micro->line_height : 0, vh = m->val[0] ? fv->line_height : 0;
		lv_area_t k = c;
		k.y2 -= ch + vh;
		th_paint_knob (l, &k, m->v, m->max, m->rgb);
		if (ch) th_text (l, &c, 0, h - ch - vh, w, TH->f_micro, oled () ? TH->dim : TH->win_dim, LV_TEXT_ALIGN_CENTER, m->cap);
		if (vh) th_text (l, &c, 0, h - vh, w, fv, oled () ? TH->ink : TH->win_ink, LV_TEXT_ALIGN_CENTER, m->val);	// the value under its label
		return;
	}
	int vert = m->kind == TH_METER_V;
	if (oled ())
	{
		// an LED ladder of dots: one dot lit, one dark, the empty part faint
		int len = vert ? h : w, n = (len + 1) / 2, lit = (v * n + m->max / 2) / (m->max ? m->max : 1);
		for (int i = 0; i < n; i++)
		{
			uint32_t col = i < lit ? m->rgb : 0x1E1E1E;
			if (vert) th_fill (l, &c, 0, h - 1 - 2 * i, w - 1, h - 1 - 2 * i, col, LV_OPA_COVER, 0);
			else th_fill (l, &c, 2 * i, 0, 2 * i, h - 1, col, LV_OPA_COVER, 0);
		}
		return;
	}
	// CLASSIC: a recessed groove in the panel, lit in the LED's colour
	th_fill (l, &c, 0, 0, w - 1, h - 1, 0x2A2C30, LV_OPA_COVER, 3);
	th_frame (l, &c, 0, 0, w - 1, h - 1, HW_EDGE, 1, 3);
	if (!v || m->v < 0) return;
	if (vert)
	{
		int fh = (h - 4) * v / m->max;
		th_fill (l, &c, 2, h - 2 - fh, w - 3, h - 3, m->rgb, LV_OPA_COVER, 2);
	}
	else
	{
		int fw = (w - 4) * v / m->max;
		th_fill (l, &c, 2, 2, 1 + fw, h - 3, m->rgb, LV_OPA_COVER, 2);
	}
}

void th_meter_value (lv_obj_t *o, const char *val)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (!strcmp (m->val, val)) return;
	copy (m->val, val, sizeof m->val);
	lv_obj_invalidate (o);
}
lv_obj_t *th_meter (lv_obj_t *parent, int x, int y, int w, int h, int max, int kind, uint32_t rgb)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	meter_t *m = (meter_t *) lv_malloc_zeroed (sizeof (meter_t));
	m->max = (int16_t) (max > 0 ? max : 1); m->kind = (int16_t) kind; m->rgb = rgb; m->v = -1;
	lv_obj_set_user_data (o, m);
	lv_obj_add_event_cb (o, meter_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_meter_set (lv_obj_t *o, int v)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (m->v == v) return;
	m->v = (int16_t) v;
	lv_obj_invalidate (o);
}

void th_meter_label (lv_obj_t *o, const char *cap)
{
	meter_t *m = (meter_t *) lv_obj_get_user_data (o);
	if (!strcmp (m->cap, cap)) return;
	copy (m->cap, cap, sizeof m->cap);
	lv_obj_invalidate (o);
}

typedef struct { int16_t v, min, max, drag; th_cb_t cb; void *ud; } slider_t;

static void slider_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	int span = s->max > s->min ? s->max - s->min : 1;
	if (oled ())
	{
		int kw = 5, x = (w - kw) * (s->v - s->min) / span, cy = h / 2;
		th_fill (l, &c, 0, cy, w - 1, cy, DOT_DOTTED, LV_OPA_COVER, 0);		// the track
		th_fill (l, &c, 0, cy - 1, x, cy + 1, TH->ink, LV_OPA_COVER, 0);		// the value
		th_fill (l, &c, x, 0, x + kw - 1, h - 1, s->drag ? TH->sel_bg : TH->ink, LV_OPA_COVER, 0);	// the handle
		th_fill (l, &c, x + 2, 2, x + 2, h - 3, 0x000000, LV_OPA_COVER, 0);
		return;
	}
	int kw = 28, x = (w - kw) * (s->v - s->min) / span, gh = h / 3;
	th_fill (l, &c, 0, (h - gh) / 2, w - 1, (h + gh) / 2, 0x2A2C30, LV_OPA_COVER, gh / 2);
	th_fill (l, &c, 2, (h - gh) / 2 + 2, x + kw / 2, (h + gh) / 2 - 2, TH->accent, LV_OPA_COVER, gh / 2);
	lv_area_t k = {c.x1 + x, c.y1, c.x1 + x + kw - 1, c.y2};
	th_paint_key (l, &k, "", 0, 0, TH_KEY, 0, s->drag, 0, 0);
}

static void slider_ev (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	lv_event_code_t code = lv_event_get_code (e);
	if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), kw = oled () ? 5 : 28;
	int v = s->min + (int) ((long) (p.x - c.x1 - kw / 2) * (s->max - s->min) / (w - kw > 0 ? w - kw : 1));
	v = v < s->min ? s->min : v > s->max ? s->max : v;
	s->drag = code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING;
	if (v != s->v || code != LV_EVENT_PRESSING)
	{
		s->v = (int16_t) v;
		inval_key (o);
		if (s->cb) s->cb (o, v | (code == LV_EVENT_RELEASED ? TH_RELEASED : 0), s->ud);
	}
}

lv_obj_t *th_slider (lv_obj_t *parent, int x, int y, int w, int h, int min, int max, th_cb_t cb, void *ud)
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area (o, TH->touch / 3);
	slider_t *s = (slider_t *) lv_malloc_zeroed (sizeof (slider_t));
	s->min = (int16_t) min; s->max = (int16_t) max; s->v = (int16_t) max; s->cb = cb; s->ud = ud;
	lv_obj_set_user_data (o, s);
	lv_obj_add_event_cb (o, slider_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (o, slider_ev, LV_EVENT_ALL, 0);
	lv_obj_add_event_cb (o, free_ud, LV_EVENT_DELETE, 0);
	return o;
}

void th_slider_set (lv_obj_t *o, int v)
{
	slider_t *s = (slider_t *) lv_obj_get_user_data (o);
	if (s->v == v || s->drag) return;
	s->v = (int16_t) v;
	inval_key (o);
}

int th_slider_get (lv_obj_t *o) { return ((slider_t *) lv_obj_get_user_data (o))->v; }
int th_slider_dragging (lv_obj_t *o) { return ((slider_t *) lv_obj_get_user_data (o))->drag; }
