// pi/ui/sidecar_rec.c - see sidecar_rec.h. A code-drawn REC / STOP key and clock, the channel grid as one code-drawn
// area (each cell's meter invalidated only when what it shows changed, so 25 views a second redraw a few strips, not
// the page), the takes as labels and keys, an overlay for the delete's confirmation. Every mark goes through th_fill /
// th_text / th_paint_key, so the OLED theme keeps its dots: there the record state is teal and brightness (no red),
// CLASSIC lights the Rytm's red record LEDs.
#include "sidecar_rec.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"	// lv_area_intersect, l->_clip_area: a cell's meter redrawn alone
#include <math.h>
#include <stdint.h>
#include <string.h>

// the grid: three columns of eight. Column 0 the Rytm's stems (IN 2..9), column 1 the Sidecar's voices in the Rytm's
// track order (the voice of each stem's track beside it: core/ui/remote.cpp kTrackVoice), column 2 the mix and the
// returns; its last cell ALL / NONE
#define ROWS	8
#define CELLS	(3 * ROWS)
#define C_ALL	(-2)
static const signed char kCell[CELLS] = {
	3, 4, 5, 6, 7, 8, 9, 10,
	13 + 0, 13 + 2, 13 + 7, 13 + 1, 13 + 3, 13 + 5, 13 + 4, 13 + 6,
	0, 1, 2, 11, 12, 21, 22, C_ALL,
};
static const char *const kGroup[3] = {"RYTM STEMS", "SIDECAR VOICES", "MIX + RETURNS"};
static const char *const kGroupC[3] = {"RYTM STEMS", "SIDECAR VOICES", "MIX AND RETURNS"};

#define DB_FLOOR	(-60.0f)
#define FALL_DB_S	24.0f
#define HOLD_MS		1500u
#define CLIP_MS		3000u

static lv_obj_t *s_Page, *s_TitleBar, *s_State, *s_RecKey, *s_ArmKey, *s_TakesKey, *s_Clock, *s_Take, *s_Status, *s_Info[4];
static lv_obj_t *s_Group[3], *s_Grid, *s_Off;
// the takes: TROWS rows on the page (s_nRows of them fit), showing takes s_nTop.. of the view's (newest first); a
// vertical drag on the rows scrolls them a row at a time
#define TROWS	8
static lv_obj_t *s_TBox, *s_THead[4], *s_TName[TROWS], *s_TLen[TROWS], *s_TSize[TROWS], *s_TFiles[TROWS], *s_TDel[TROWS], *s_TEmpty, *s_THit[TROWS], *s_StopKey;
static lv_obj_t *s_Dlg, *s_DlgText[3], *s_DlgYes, *s_DlgNo, *s_Scrim;
static sidecar_rec_cmd_t s_Cmd;
static sidecar_rec_view_t s_V;
static int s_bHave, s_bTakes, s_nDel = -1, s_nPress = -1, s_nRows = TROWS;	// s_nRows: the takes' rows that fit
static int s_nTop, s_nRowH = 1, s_nDragY = -1, s_nDragTop, s_bDragged;	// the first take shown; a drag's start
// the dialog: a take's delete, or the warning before a take (the selection needs most of the destination's speed)
enum { DLG_NONE, DLG_DEL, DLG_WARN };
static int s_nDlg;
static char s_DlgCmd[16];				// DLG_WARN: what its RECORD / ARM key sends
static char s_DelName[SRV_NAME];			// DLG_DEL: the take asked about (by name: the list can change under it)
static int ntakes (void);
static void rec_go (const char *c);
static unsigned s_nTakesGen = ~0u;
static uint32_t s_nLastMs;
static int s_nStatusW = -1, s_nValidWas = -1;		// CLASSIC's status line width; the last view's valid
static int s_nPlRow = -1, s_nPlSt, s_nPlPx = -1;		// the take playing / paused: its visible row, state, the bar's filled width
static uint32_t s_nPlAt;				// the progress's last update (<= 10 a second)
static unsigned s_nClock = ~0u;				// what the clock shows (its redraw only on a change)
// the meters: per source and side, in dB; the cells' last drawn state
static float s_Disp[SRV_CH][2], s_Hold[SRV_CH][2];
static uint32_t s_HoldAt[SRV_CH][2], s_ClipAt[SRV_CH];
typedef struct { signed char lit[2], hold[2]; unsigned char clip, armed, press, en; } cell_t;
static cell_t s_Drawn[CELLS];

static int oled (void) { return TH->id == THEME_OLED; }
static const lv_font_t *mi_font (void) { return oled () ? TH->f_micro : &lv_font_montserrat_14; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }
static int busy (void) { return s_V.state == SRV_REC || s_V.state == SRV_SAVING; }
int sidecar_rec_stereo (int id) { return id == 0 || id == 21 || id == 22; }

static lv_obj_t *label_w (lv_obj_t *p, int x, int y, int w, const lv_font_t *f, uint32_t rgb, int align)
{
	lv_obj_t *l = th_label (p, x, y, f, rgb, "");
	lv_obj_set_size (l, w, lv_font_get_line_height (f));		// one line, cut with dots
	lv_label_set_long_mode (l, LV_LABEL_LONG_MODE_DOTS);
	if (align) lv_obj_set_style_text_align (l, align > 0 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
	return l;
}

// ---- geometry (the 800x480 layouts plus what the screen adds: TH->dx, dy)
static int W (void) { return oled () ? TH->w - 48 : TH->w; }
static int grid_y (void) { return oled () ? 72 : 88; }
static int grid_h (void) { return oled () ? 168 + TH->dy : 336 + TH->dy; }
static int pitch (void) { return grid_h () / ROWS; }			// 21 / 28 dots, 42 / 57 px
static int col_w (void) { return oled () ? (W () - 4) / 3 : (W () - 16 - 16) / 3; }
static int col_x (int c) { return oled () ? c * (col_w () + 2) : 8 + c * (col_w () + 8); }
// a cell's meter: its x range and the bars' rows, in the grid's coordinates
static void meter_geo (int c, int r, int *x0, int *x1, int *y0, int *bh, int *clipx)
{
	int x = col_x (c), y = r * pitch (), w = col_w (), h = pitch ();
	if (oled ())
	{
		*x0 = x + 50 + TH->dx / 12, *x1 = x + w - 9, *clipx = x + w - 6;
		*bh = h >= 26 ? 7 : 5;
		*y0 = y + (h - *bh) / 2;
	}
	else
	{
		*x0 = x + 112 + TH->dx / 12, *x1 = x + w - 40, *clipx = *x1 + 4;	// the clip lamp inside the window
		*bh = 12 + TH->dy / 20;
		*y0 = y + (h - *bh) / 2;
	}
}
// what a cell's meter changes (bars, hold, clip lamp), in the grid's coordinates: redrawn alone when only it moved
static lv_area_t meter_dyn (int k)
{
	int x0, x1, y0, bh, cx;
	meter_geo (k / ROWS, k % ROWS, &x0, &x1, &y0, &bh, &cx);
	lv_area_t a = {x0, y0, cx + 3, y0 + bh - 1};
	if (!oled ()) a.x1 = x0 - 1, a.y1 = y0 - 1, a.x2 = cx + 7, a.y2 = y0 + bh;	// inside the window's opaque fill
	return a;
}
static int nseg (int x0, int x1) { return (x1 - x0 + 1) / (oled () ? 3 : 4); }
static int seg_of (float db, int n)
{
	if (db <= DB_FLOOR) return 0;
	int k = (int) ((db - DB_FLOOR) / -DB_FLOOR * n + 0.5f);
	return k < 0 ? 0 : k > n ? n : k;
}

// ---- drawing helpers
static void disc (lv_layer_t *l, const lv_area_t *c, int cx, int cy, int r, uint32_t rgb)	// OLED: rows of dots
{
	if (!oled ()) { th_fill (l, c, cx - r, cy - r, cx + r, cy + r, rgb, LV_OPA_COVER, LV_RADIUS_CIRCLE); return; }
	for (int y = -r; y <= r; y++)
	{
		int x = 0;
		while ((x + 1) * (x + 1) + y * y <= r * r + r) x++;
		th_fill (l, c, cx - x, cy + y, cx + x, cy + y, rgb, LV_OPA_COVER, 0);
	}
}

// segment digits (original drawing): w x h, strokes t thick; a b c d e f g as on any 7-segment display, the unlit
// segments drawn dark as on a real LED window
static void digit (lv_layer_t *l, const lv_area_t *c, int x, int y, int w, int h, int t, int d, uint32_t on, uint32_t off)
{
	static const unsigned char kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
	int m = d < 0 ? 0 : kSeg[d % 10], hh = (h - t) / 2;
	struct { int x1, y1, x2, y2; } s[7] = {
		{x + t, y, x + w - 1 - t, y + t - 1},				// a
		{x + w - t, y + t, x + w - 1, y + hh - 1 + t / 2},		// b
		{x + w - t, y + hh + t, x + w - 1, y + h - 1 - t},		// c
		{x + t, y + h - t, x + w - 1 - t, y + h - 1},			// d
		{x, y + hh + t, x + t - 1, y + h - 1 - t},			// e
		{x, y + t, x + t - 1, y + hh - 1 + t / 2},			// f
		{x + t, y + hh, x + w - 1 - t, y + hh + t - 1},			// g
	};
	for (int k = 0; k < 7; k++) th_fill (l, c, s[k].x1, s[k].y1, s[k].x2, s[k].y2, m >> k & 1 ? on : off, LV_OPA_COVER, 0);
}

static void clock_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), st = s_V.state;
	int dw = O ? 10 : 20, dh = O ? 18 : 34, t = O ? 2 : 4, gap = O ? 2 : 6, cw = O ? 4 : 10;
	uint32_t on, off = O ? 0x1C1C1C : 0x1A1C1F;
	if (O) on = st == SRV_REC ? TH->accent_hi : st == SRV_ARMED ? TH->ink : s_V.take[0] ? TH->ink : TH->dim;
	else on = st == SRV_REC ? TH->bad : st == SRV_ARMED ? TH->warn : TH->win_ink;
	unsigned s = st == SRV_ARMED ? 0 : s_V.secs, hr = s / 3600 % 10, mn = s / 60 % 60, sc = s % 60;
	int x = 0, y = (lv_area_get_height (&c) - dh) / 2;
	int dd[5] = {(int) hr, (int) (mn / 10), (int) (mn % 10), (int) (sc / 10), (int) (sc % 10)};
	for (int i = 0; i < 5; i++)
	{
		digit (l, &c, x, y, dw, dh, t, dd[i], on, off);
		x += dw + gap;
		if (i == 0 || i == 2)			// the colons
		{
			int cx = x + (cw - t) / 2 - gap / 2;
			th_fill (l, &c, cx, y + dh / 3 - t / 2, cx + t - 1, y + dh / 3 + t / 2 - (O ? 0 : 1), on, LV_OPA_COVER, 0);
			th_fill (l, &c, cx, y + 2 * dh / 3 - t / 2, cx + t - 1, y + 2 * dh / 3 + t / 2 - (O ? 0 : 1), on, LV_OPA_COVER, 0);
			x += cw;
		}
	}
}

// REC / STOP: the key's face (th_paint_key), a record mark over it
static void rec_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), O = oled (), st = s_V.state;
	int pressed = lv_obj_has_state (o, LV_STATE_PRESSED), dis = !s_V.valid || st == SRV_SAVING;
	int rec = st == SRV_REC, blink = (lv_tick_get () / 500) & 1;
	const char *txt = rec ? "STOP" : st == SRV_SAVING ? "SAVING" : st == SRV_ARMED ? "REC NOW" : "REC";
	const lv_font_t *f = O ? TH->f_body : &lv_font_montserrat_16;
	int fh = lv_font_get_line_height (f), mr = O ? 7 : 12, my = (h - (2 * mr + 1) - 3 - fh) / 2 + mr;
	uint32_t ink;
	if (O)
	{
		if (rec && !pressed)			// recording: the key lit teal, its print black
		{
			th_fill (l, &c, 0, 0, w - 1, h - 1, TH->accent, LV_OPA_COVER, 0);
			th_frame (l, &c, 0, 0, w - 1, h - 1, TH->accent_hi, 1, 0);
			ink = 0x000000;
		}
		else
		{
			th_paint_key (l, &c, "", 0, f, TH_KEY_ACCENT, 0, pressed, dis, 0);
			ink = pressed ? 0x000000 : dis ? TH->faint : TH->ink;
		}
		if (rec) th_fill (l, &c, w / 2 - 5, my - 5, w / 2 + 5, my + 5, ink, LV_OPA_COVER, 0);		// stop: a square
		else if (st == SRV_ARMED)						// armed: a ring, its centre blinking
		{
			disc (l, &c, w / 2, my, mr, ink);
			disc (l, &c, w / 2, my, mr - 2, pressed ? DOT_DITHER : 0x000000);
			if (blink) disc (l, &c, w / 2, my, mr - 4, TH->accent_hi);
		}
		else disc (l, &c, w / 2, my, mr, ink);
	}
	else
	{
		th_paint_key (l, &c, "", 0, f, TH_KEY, 0, pressed, dis, rec || (st == SRV_ARMED && blink) ? LED_REC : 0);
		ink = dis ? 0x6E7176 : 0xE8E8E8;
		uint32_t red = 0xFF3B30;
		if (rec)
			th_fill (l, &c, w / 2 - 9, my - 9, w / 2 + 9, my + 9, 0xF0F0F0, LV_OPA_COVER, 2);
		else
		{
			uint32_t r = dis ? 0x5A3030 : st == SRV_ARMED && !blink ? 0x7A2420 : red;
			if (!dis) disc (l, &c, w / 2, my, mr + 4, th_mix (0x2B2D30, r, 70));	// the glow
			disc (l, &c, w / 2, my, mr, r);
		}
	}
	th_text (l, &c, 0, my + mr + 3, w, f, ink, LV_TEXT_ALIGN_CENTER, txt);
}

static void rec_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_invalidate (s_RecKey);
	if (code != LV_EVENT_CLICKED || !s_V.valid) return;
	if (s_V.state == SRV_REC) cmd ("rec=stop");
	else if (s_V.state != SRV_SAVING) rec_go ("rec=start");
}

// ---- the grid
static void cell_state (int k, cell_t *o)
{
	memset (o, 0, sizeof *o);
	int id = kCell[k], c = k / ROWS, r = k % ROWS;
	o->press = s_nPress == k;
	o->en = s_V.valid && !busy () && s_V.state != SRV_ARMED;
	if (id == C_ALL) return;
	o->armed = (s_V.mask >> id & 1) != 0;
	int x0, x1, y0, bh, cx;
	meter_geo (c, r, &x0, &x1, &y0, &bh, &cx);
	int n = nseg (x0, x1);
	for (int s = 0; s < 1 + sidecar_rec_stereo (id); s++)
	{
		o->lit[s] = (signed char) seg_of (s_Disp[id][s], n);
		o->hold[s] = (signed char) seg_of (s_Hold[id][s], n);
	}
	o->clip = lv_tick_get () - s_ClipAt[id] < CLIP_MS && s_ClipAt[id] != 0;
}

static void bar (lv_layer_t *l, const lv_area_t *c, int x0, int y, int h, int n, int lit, int hold, int armed)
{
	int O = oled (), sw = O ? 2 : 3, sp = O ? 3 : 4;
	for (int i = 0; i < n; i++)
	{
		float db = DB_FLOOR + (i + 1) * -DB_FLOOR / n;		// the segment's top
		uint32_t col;
		if (i < lit || i == hold - 1)
		{
			if (O) col = !armed ? (i == hold - 1 ? TH->dim : TH->faint) : db > -6.0f ? TH->accent : db > -18.0f || i == hold - 1 ? TH->ink : TH->dim;
			else
			{
				col = db > -3.0f ? 0xFF4A3D : db > -12.0f ? 0xE8B040 : 0x34C85A;
				if (!armed) col = th_mix (0x1A1A1A, col, 110);
				if (i == hold - 1 && i >= lit) col = armed ? 0xF0F0F0 : 0x8A8E94;
			}
		}
		else col = O ? 0x262626 : 0x1E2022;
		th_fill (l, c, x0 + i * sp, y, x0 + i * sp + sw - 1, y + h - 1, col, LV_OPA_COVER, 0);
	}
}

static void grid_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), ph = pitch (), cw = col_w ();
	const lv_font_t *f = O ? TH->f_body : &lv_font_montserrat_16;
	int fh = lv_font_get_line_height (f);
	const lv_area_t *clip = &l->_clip_area;
	for (int k = 0; k < CELLS; k++)
	{
		int col = k / ROWS, r = k % ROWS, id = kCell[k];
		int x = col_x (col), y = r * ph;
		lv_area_t ca = {c.x1 + x - 4, c.y1 + y, c.x1 + x + cw + 3, c.y1 + y + ph + 3}, tmp;
		if (!lv_area_intersect (&tmp, &ca, clip)) continue;		// (a meter's redraw: the other cells untouched)
		cell_t cs;
		cell_state (k, &cs);
		int x0, x1, y0, bh, clipx;
		meter_geo (col, r, &x0, &x1, &y0, &bh, &clipx);
		lv_area_t md = meter_dyn (k);
		lv_area_move (&md, c.x1, c.y1);
		int only = id != C_ALL && lv_area_is_in (clip, &md, 0);		// the meter alone
		if (only)
		{
			if (!O) th_fill (l, &c, md.x1 - c.x1, md.y1 - c.y1, md.x2 - c.x1, md.y2 - c.y1, 0x08090A, LV_OPA_COVER, 0);
			else if (cs.press && cs.en) th_fill (l, &c, md.x1 - c.x1, md.y1 - c.y1, md.x2 - c.x1, md.y2 - c.y1, DOT_DITHER, LV_OPA_COVER, 0);
			goto meter;
		}
		if (id == C_ALL)		// ALL / NONE: a plain key
		{
			int all = (s_V.mask & 0x7FFFFFu) == 0x7FFFFFu;
			lv_area_t a = {c.x1 + x + (O ? 0 : 0), c.y1 + y + (O ? 1 : 2), c.x1 + x + cw - 1, c.y1 + y + ph - (O ? 2 : 3)};
			th_paint_key (l, &a, all ? "NONE" : "ALL", 0, O ? TH->f_body : &lv_font_montserrat_16, TH_KEY, 0, cs.press, !cs.en, 0);
			continue;
		}
		const char *name = s_V.name[id][0] ? s_V.name[id] : "-";
		if (O)
		{
			if (cs.press && cs.en) th_fill (l, &c, x, y + 1, x + cw - 1, y + ph - 2, DOT_DITHER, LV_OPA_COVER, 0);
			uint32_t ink = cs.press && cs.en ? 0x000000 : cs.armed ? TH->ink : busy () ? TH->faint : TH->dim;
			int sq = (ph - 7) / 2;
			if (cs.armed) th_fill (l, &c, x + 3, y + sq, x + 9, y + sq + 6, s_V.state == SRV_REC ? TH->accent_hi : TH->accent, LV_OPA_COVER, 0);
			else th_frame (l, &c, x + 3, y + sq, x + 9, y + sq + 6, busy () ? 0x3C3C3C : TH->faint, 1, 0);
			th_text (l, &c, x + 12, y + (ph - fh) / 2 + 1, x0 - x - 14, f, ink, LV_TEXT_ALIGN_LEFT, name);
			if (r < ROWS - 1 && !(cs.press && cs.en))	// a faint rule between rows
				for (int dx = x + 13; dx < x + cw - 2; dx += 2) th_fill (l, &c, dx, y + ph - 1, dx, y + ph - 1, 0x262626, LV_OPA_COVER, 0);
		}
		else
		{
			lv_area_t a = {c.x1 + x, c.y1 + y + 2, c.x1 + x + cw - 1, c.y1 + y + ph - 3};
			th_paint_key (l, &a, "", 0, f, TH_KEY, 0, cs.press && cs.en, 0, 0);
			int ly = y + ph / 2;
			uint32_t led = cs.armed ? (s_V.state == SRV_REC ? 0xFF3B30 : 0xD83A2E) : 0x4C4E52;
			if (cs.armed) th_fill (l, &c, x + 8, ly - 9, x + 26, ly + 9, th_mix (0x2B2D30, led, 80), LV_OPA_COVER, LV_RADIUS_CIRCLE);
			th_fill (l, &c, x + 12, ly - 5, x + 22, ly + 5, led, LV_OPA_COVER, LV_RADIUS_CIRCLE);
			th_text (l, &c, x + 32, y + (ph - fh) / 2, x0 - x - 40, f, cs.armed ? 0xF0F0F0 : busy () ? 0x6E7176 : 0xB8BBC0,
				 LV_TEXT_ALIGN_LEFT, name);
			th_fill (l, &c, x0 - 4, y0 - 4, clipx + 10, y0 + bh + 3, 0x08090A, LV_OPA_COVER, 3);	// the meter's window
			th_frame (l, &c, x0 - 4, y0 - 4, clipx + 10, y0 + bh + 3, 0x3A3C40, 1, 3);
		}
	meter:;
		int n = nseg (x0, x1), st = sidecar_rec_stereo (id);
		if (st)
		{
			int bh2 = (bh - 1) / 2;
			bar (l, &c, x0, y0, bh2, n, cs.lit[0], cs.hold[0], cs.armed);
			bar (l, &c, x0, y0 + bh - bh2, bh2, n, cs.lit[1], cs.hold[1], cs.armed);
		}
		else bar (l, &c, x0, y0, bh, n, cs.lit[0], cs.hold[0], cs.armed);
		if (O) th_fill (l, &c, clipx, y0, clipx + 3, y0 + bh - 1, cs.clip ? TH->accent_hi : 0x262626, LV_OPA_COVER, 0);
		else th_fill (l, &c, clipx, y0, clipx + 6, y0 + bh - 1, cs.clip ? 0xFF3B30 : 0x3A2A2A, LV_OPA_COVER, 1);
	}
}

static void cell_inval (int k)
{
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	int x = col_x (k / ROWS), y = (k % ROWS) * pitch ();
	lv_area_t a = {c.x1 + x - 4, c.y1 + y, c.x1 + x + col_w () + 3, c.y1 + y + pitch () + 3};	// (CLASSIC's key shadow)
	lv_obj_invalidate_area (s_Grid, &a);
}
static void meter_inval (int k)
{
	lv_area_t c, a = meter_dyn (k);
	lv_obj_get_coords (s_Grid, &c);
	lv_area_move (&a, c.x1, c.y1);
	lv_obj_invalidate_area (s_Grid, &a);
}

static int cell_at (lv_point_t p)
{
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	int x = p.x - c.x1, y = p.y - c.y1, r = y / pitch ();
	if (y < 0 || r >= ROWS) return -1;
	for (int col = 0; col < 3; col++)
		if (x >= col_x (col) - (oled () ? 1 : 4) && x < col_x (col) + col_w () + (oled () ? 1 : 4)) return col * ROWS + r;
	return -1;
}

static void grid_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	if (code == LV_EVENT_PRESSED)
	{
		s_nPress = cell_at (p);
		if (s_nPress >= 0) cell_inval (s_nPress);
		return;
	}
	if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
	int k = s_nPress;
	s_nPress = -1;
	if (k < 0) return;
	cell_inval (k);
	if (code != LV_EVENT_RELEASED || cell_at (p) != k) return;
	if (!s_V.valid || busy () || s_V.state == SRV_ARMED) return;	// the recorder takes a selection only when idle
	int id = kCell[k];
	char b[24];
	if (id == C_ALL) lv_snprintf (b, sizeof b, "rec=ch:%s", (s_V.mask & 0x7FFFFFu) == 0x7FFFFFu ? "none" : "all");
	else lv_snprintf (b, sizeof b, "rec=ch:%c%d", s_V.mask >> id & 1 ? '-' : '+', id);
	cmd (b);
}

// ---- the takes
static void size_text (char *b, int n, unsigned kb)
{
	if (kb >= 10u * 1024 * 1024) lv_snprintf (b, n, "%u GB", kb >> 20);
	else if (kb >= 1024 * 1024) lv_snprintf (b, n, "%u.%u GB", kb >> 20, (kb & 0xFFFFF) * 10 >> 20);
	else if (kb >= 10 * 1024) lv_snprintf (b, n, "%u MB", kb >> 10);
	else if (kb >= 1024) lv_snprintf (b, n, "%u.%u MB", kb >> 10, (kb & 1023) * 10 >> 10);
	else lv_snprintf (b, n, "%u KB", kb);
}
static void len_text (char *b, int n, unsigned s)
{
	if (s >= 3600) lv_snprintf (b, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else lv_snprintf (b, n, "%u:%02u", s / 60, s % 60);
}

static void dlg_keys (int warn)
{
	int O = oled ();
	th_key_text (s_DlgYes, warn ? (!strcmp (s_DlgCmd, "rec=arm") ? (O ? "ARM" : "Arm") : (O ? "RECORD" : "Record")) : O ? "DELETE" : "Delete");
	th_key_kind (s_DlgYes, (warn ? TH_KEY : TH_KEY_DANGER) | TH_KEY_FILL);
}
static void dlg_show (int i)
{
	s_nDel = i;
	if (i < 0 || i >= ntakes ())
	{
		s_nDel = -1, s_nDlg = DLG_NONE;
		lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	s_nDlg = DLG_DEL;
	dlg_keys (0);
	lv_snprintf (s_DelName, sizeof s_DelName, "%s", s_V.takes[i].name);
	const srv_take_t *t = &s_V.takes[i];
	char b[96], sz[16], ln[16];
	size_text (sz, sizeof sz, t->kb);
	len_text (ln, sizeof ln, t->secs);
	char nm[SRV_NAME];		// a long name cut in the middle: its start and its date both show
	const lv_font_t *f0 = oled () ? TH->f_body : &lv_font_montserrat_22;
	th_fit_middle (nm, sizeof nm, f0, t->name, (oled () ? 284 : 472) - th_text_width (f0, oled () ? "DELETE ?" : "Delete ?"));
	lv_snprintf (b, sizeof b, oled () ? "DELETE %s?" : "Delete %s?", nm);
	th_label_set (s_DlgText[0], b);
	lv_snprintf (b, sizeof b, oled () ? "%d FILES, %s, %s" : "%d files, %s, %s long", t->files, sz, ln);
	th_label_set (s_DlgText[1], b);
	int drv = strchr (t->name, ':') != 0;	// a drive's take ("USB:take_004")
	th_label_set (s_DlgText[2], oled () ? (drv ? "GONE FROM THE DRIVE: NO UNDO" : "GONE FROM THE CARD: NO UNDO")
		      : drv ? "Removed from the drive. This cannot be undone." : "Removed from the card. This cannot be undone.");
	lv_obj_remove_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
}
static void mbs (char *b, int n, unsigned kbps) { lv_snprintf (b, n, "%u.%u MB/s", kbps / 1024, kbps % 1024 * 10 / 1024); }
// the warning before a take: c the command its key sends ("rec=start", "rec=arm")
static void warn_show (const char *c)
{
	int O = oled (), drv = s_V.vol[0] && strcmp (s_V.vol, "SD") != 0;
	char b[112], nd[16], sp[16];
	lv_snprintf (s_DlgCmd, sizeof s_DlgCmd, "%s", c);
	s_nDel = -1, s_nDlg = DLG_WARN;
	dlg_keys (1);
	mbs (nd, sizeof nd, s_V.need_kbps);
	mbs (sp, sizeof sp, s_V.speed_kbps);
	if (O) lv_snprintf (b, sizeof b, drv ? "THE %s DRIVE MAY FALL BEHIND" : "THE CARD MAY FALL BEHIND", s_V.vol);
	else lv_snprintf (b, sizeof b, drv ? "The %s drive may fall behind" : "The SD card may fall behind", s_V.vol);
	th_label_set (s_DlgText[0], b);
	lv_snprintf (b, sizeof b, O ? "NEEDS %s: %u %% OF ITS %s %s" : "This take needs %s, %u %% of its %s (%s).", nd, s_V.warn_pct, sp,
		     s_V.speed_measured ? (O ? "(BENCH)" : "measured") : (O ? "(GUESS)" : "not measured: STATUS > Storage bench"));
	for (char *q = b; O && *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;	// (MB/S on the OLED)
	th_label_set (s_DlgText[1], b);
	th_label_set (s_DlgText[2], O ? "BLOCKS IT MISSES ARE SILENCE" : "Missed blocks are written as silence: fewer channels?");
	lv_obj_remove_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
}
// REC / ARM from idle: asked first when the selection needs more than ~70 % of the destination's speed
static void rec_go (const char *c)
{
	if (s_V.state == SRV_IDLE && s_V.warn_pct) warn_show (c);
	else cmd (c);
}

static void del_cb (lv_obj_t *o, int idx, void *ud) { (void) o; (void) idx; if (!busy ()) dlg_show (s_nTop + (int) (intptr_t) ud); }
static void dlg_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	if (ud && s_nDlg == DLG_WARN && s_V.state == SRV_IDLE) cmd (s_DlgCmd);
	else if (ud && s_nDlg == DLG_DEL && s_DelName[0] && !busy ())
	{
		char b[96];
		lv_snprintf (b, sizeof b, "rec=del:%s", s_DelName);
		cmd (b);
	}
	dlg_show (-1);
}

static void scrim_cb (lv_event_t *e) { (void) e; dlg_show (-1); }

// ---- playback: a tap on a take's row plays / pauses it (another take: switches), STOP resets. The glyph, the row's
// frame and the thin progress bar are the row's own picture; the bar alone is redrawn as the position moves.
static int ntakes (void) { return s_V.ntakes < 0 ? 0 : s_V.ntakes > SRV_TAKES ? SRV_TAKES : s_V.ntakes; }
static int top_max (void) { return ntakes () > s_nRows ? ntakes () - s_nRows : 0; }
static int rows_on (void) { int n = ntakes () - s_nTop; return n < 0 ? 0 : n > s_nRows ? s_nRows : n; }	// rows showing a take
static int pl_row (void)	// the playing take's row (-1: none, or scrolled away)
{
	if (s_V.pl_state == SRV_PL_NONE) return -1;
	int n = rows_on ();
	for (int r = 0; r < n; r++)
		if (!strcmp (s_V.takes[s_nTop + r].name, s_V.pl_take)) return r;
	return -1;
}
static int bar_h (void) { return oled () ? 2 : 4; }
static int bar_x0 (void) { return oled () ? 16 : 32; }
static int bar_px (int w)	// the filled width of a bar of w
{
	if (s_V.pl_state == SRV_PL_NONE || !s_V.pl_len_ms) return 0;
	unsigned long long p = s_V.pl_pos_ms;
	if (p > s_V.pl_len_ms) p = s_V.pl_len_ms;
	return (int) (p * (unsigned) w / s_V.pl_len_ms);
}
static int bar_w (lv_obj_t *o) { return lv_obj_get_width (o) - bar_x0 () - (oled () ? 4 : 14); }
static void bar_inval (int i)
{
	lv_area_t c;
	lv_obj_get_coords (s_THit[i], &c);
	lv_area_t a = {c.x1 + bar_x0 (), c.y2 - bar_h () - (oled () ? 1 : 3), c.x2 - 1, c.y2 - (oled () ? 0 : 2)};
	lv_obj_invalidate_area (s_THit[i], &a);
}
static void glyph (lv_layer_t *l, const lv_area_t *c, int x, int cy, int gh, int pause, uint32_t rgb)
{
	if (pause)
	{
		int bw = gh / 3 > 2 ? gh / 3 : 2;
		th_fill (l, c, x, cy - gh / 2, x + bw - 1, cy - gh / 2 + gh - 1, rgb, LV_OPA_COVER, 0);
		th_fill (l, c, x + 2 * bw, cy - gh / 2, x + 3 * bw - 1, cy - gh / 2 + gh - 1, rgb, LV_OPA_COVER, 0);
		return;
	}
	int half = (gh - 1) / 2;
	for (int r = 0; r < gh; r++)		// a right-pointing triangle, row by row
	{
		int d = r <= half ? r : gh - 1 - r, w = 1 + d * (oled () ? 1 : 2);
		th_fill (l, c, x, cy - gh / 2 + r, x + w - 1, cy - gh / 2 + r, rgb, LV_OPA_COVER, 0);
	}
}
static void hit_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	int i = (int) (intptr_t) lv_event_get_user_data (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), O = oled();
	int on = i == s_nPlRow, paused = on && s_V.pl_state == SRV_PL_PAUSED;
	int pressed = lv_obj_has_state (o, LV_STATE_PRESSED) && !busy ();
	int gh = O ? 9 : 14, gx = O ? 4 : 6;
	if (O)
	{
		if (pressed) th_fill (l, &c, 0, 0, w - 1, h - 1, DOT_SHADE, LV_OPA_COVER, 0);
		if (on) th_frame (l, &c, 0, 0, w - 1, h - 1, TH->accent_hi, 1, 0);
		glyph (l, &c, gx, h / 2, gh, paused, on ? (paused ? TH->ink : TH->accent_hi) : TH->faint);
		if (on)		// the bar: a dotted track, the played part solid
		{
			int bw = bar_w (o), y = h - 1 - 2;
			th_fill (l, &c, bar_x0 (), y, bar_x0 () + bw - 1, y + 1, DOT_DOTTED, LV_OPA_COVER, 0);
			int px = bar_px (bw);
			if (px > 0) th_fill (l, &c, bar_x0 (), y, bar_x0 () + px - 1, y + 1, TH->accent_hi, LV_OPA_COVER, 0);
		}
	}
	else
	{
		if (on) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (TH->win, TH->accent, 64), LV_OPA_COVER, 4);
		if (pressed) th_fill (l, &c, 0, 0, w - 1, h - 1, th_mix (TH->win, 0xFFFFFF, 40), LV_OPA_COVER, 4);
		glyph (l, &c, gx, h / 2 - 1, gh, paused, on ? (paused ? TH->win_ink : TH->accent_hi) : TH->win_dim);
		if (on)
		{
			int bw = bar_w (o), y = h - 3 - bar_h ();
			th_fill (l, &c, bar_x0 (), y, bar_x0 () + bw - 1, y + bar_h () - 1, th_mix (TH->win, TH->win_dim, 90), LV_OPA_COVER, 2);
			int px = bar_px (bw);
			if (px > 0) th_fill (l, &c, bar_x0 (), y, bar_x0 () + px - 1, y + bar_h () - 1, TH->accent_hi, LV_OPA_COVER, 2);
		}
	}
}
static void takes_update (void);
static void hit_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	int i = s_nTop + (int) (intptr_t) lv_event_get_user_data (e);
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) lv_obj_invalidate (lv_event_get_target_obj (e));
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)	// a vertical drag: the list a row at a time
	{
		lv_point_t p;
		lv_indev_get_point (lv_indev_active (), &p);
		if (code == LV_EVENT_PRESSED) { s_nDragY = p.y, s_nDragTop = s_nTop, s_bDragged = 0; return; }
		int dy = p.y - s_nDragY;
		if (s_nDragY < 0 || (!s_bDragged && (dy < 0 ? -dy : dy) < s_nRowH / 2)) return;
		s_bDragged = 1;
		int top = s_nDragTop - (dy + (dy < 0 ? -s_nRowH / 2 : s_nRowH / 2)) / s_nRowH;	// up: the older takes
		if (top > top_max ()) top = top_max ();
		if (top < 0) top = 0;
		if (top != s_nTop) { s_nTop = top; takes_update (); }
		return;
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_nDragY = -1;
	if (code != LV_EVENT_CLICKED || s_bDragged || busy () || i < 0 || i >= ntakes ()) return;
	char b[96];
	lv_snprintf (b, sizeof b, "play=toggle:%s", s_V.takes[i].name);
	cmd (b);
}
static void stop_cb (lv_obj_t *o, int idx, void *ud) { (void) o; (void) idx; (void) ud; if (s_V.pl_state != SRV_PL_NONE) cmd ("play=stop"); }
static void pl_time (char *b, int n)
{
	char a[16], t[16];
	len_text (a, sizeof a, s_V.pl_pos_ms / 1000);
	len_text (t, sizeof t, s_V.pl_len_ms / 1000);
	lv_snprintf (b, n, "%s / %s", a, t);
}

static void takes_update (void)
{
	char b[48];
	if (s_nTop > top_max ()) s_nTop = top_max ();
	int n = rows_on (), O = oled ();
	s_nPlRow = pl_row (), s_nPlSt = s_V.pl_state, s_nPlPx = -1;
	// the header: which takes show, when they do not all fit
	if (ntakes () > s_nRows) lv_snprintf (b, sizeof b, O ? "TAKE %d-%d/%d" : "Take   %d-%d of %d (drag to scroll)", s_nTop + 1, s_nTop + n, ntakes ());
	else lv_snprintf (b, sizeof b, O ? "TAKE" : "Take");
	th_label_set (s_THead[0], b);
	for (int i = 0; i < TROWS; i++)
	{
		int on = i < n, pl = i == s_nPlRow;
		lv_obj_t *row[6] = {s_TName[i], s_TLen[i], s_TSize[i], s_TFiles[i], s_TDel[i], s_THit[i]};
		for (int k = 0; k < 6; k++)
			if (on && !(pl && (k == 2 || k == 3))) lv_obj_remove_flag (row[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (row[k], LV_OBJ_FLAG_HIDDEN);
		if (!on) continue;
		lv_obj_invalidate (s_THit[i]);
		const srv_take_t *t = &s_V.takes[s_nTop + i];
		int live = busy () && !strcmp (t->name, s_V.take);
		th_label_fit (s_TName[i], t->name);
		th_label_color (s_TName[i], pl || (s_nTop + i == 0 && !strcmp (t->name, s_V.take)) ? (O ? 0xFFFFFF : TH->accent_hi) : O ? TH->ink : TH->win_ink);	// (L2: playing white)
		// the playing row: its length column becomes "elapsed / length" over the size and files columns
		lv_obj_set_width (s_TLen[i], pl ? lv_obj_get_width (s_THit[i]) - (lv_obj_get_x (s_TLen[i]) - lv_obj_get_x (s_THit[i])) - 4 : oled () ? 48 : 110);
		th_label_color (s_TLen[i], pl ? (oled () ? 0xFFFFFF : TH->accent_hi) : oled () ? TH->ink : TH->win_ink);
		if (pl) pl_time (b, sizeof b); else len_text (b, sizeof b, t->secs);
		th_label_set (s_TLen[i], b);
		size_text (b, sizeof b, t->kb);
		th_label_set (s_TSize[i], b);
		lv_snprintf (b, sizeof b, "%d", t->files);
		th_label_set (s_TFiles[i], b);
		th_key_enable (s_TDel[i], !busy () && !live);
	}
	th_key_enable (s_StopKey, s_V.pl_state != SRV_PL_NONE);
	th_key_led (s_StopKey, s_V.pl_state == SRV_PL_PLAYING ? LED_PLAY : 0);
	th_label_set (s_TEmpty, s_V.ntakes < 0 ? (oled () ? "READING THE CARD..." : "Reading the card...")
		      : n == 0 ? (oled () ? "NO TAKES YET: TAP REC, OR ARM AND PLAY" : "No takes yet: tap REC, or ARM and press PLAY on the Rytm") : "");
	if (s_nDlg == DLG_DEL)		// the take asked about gone from the list (deleted elsewhere, its drive pulled): no dialog
	{
		int k = 0;
		while (k < ntakes () && strcmp (s_V.takes[k].name, s_DelName)) k++;
		if (k == ntakes ()) dlg_show (-1);
	}
}

static void view_update (void)
{
	int show = s_bTakes;
	if (show) lv_obj_add_flag (s_Grid, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Grid, LV_OBJ_FLAG_HIDDEN);
	if (show) lv_obj_remove_flag (s_TBox, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_TBox, LV_OBJ_FLAG_HIDDEN);
	for (int g = 0; g < 3; g++)
		if (s_Group[g]) { if (show) lv_obj_add_flag (s_Group[g], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Group[g], LV_OBJ_FLAG_HIDDEN); }
	th_key_check (s_TakesKey, show);
	if (show) { lv_obj_add_flag (s_ArmKey, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag (s_StopKey, LV_OBJ_FLAG_HIDDEN); }	// STOP takes ARM's place
	else { lv_obj_remove_flag (s_ArmKey, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag (s_StopKey, LV_OBJ_FLAG_HIDDEN); }
	if (!show) dlg_show (-1);
}

static void takes_cb (lv_obj_t *o, int idx, void *ud) { (void) o; (void) idx; (void) ud; sidecar_rec_takes (!s_bTakes); }
static void arm_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx; (void) ud;
	if (!s_V.valid || busy ()) return;
	if (s_V.state == SRV_ARMED) cmd ("rec=stop");
	else rec_go ("rec=arm");
}

// ---- building
void sidecar_rec_create (void *parent, int y0, sidecar_rec_cmd_t c)
{
	int O = oled ();
	const int dx = TH->dx, dy = TH->dy, Wd = W (), H = O ? TH->h : 428 + dy;
	s_Cmd = c;
	s_bTakes = 0, s_nDel = -1, s_nPress = -1, s_nTakesGen = ~0u, s_nRows = TROWS, s_nTop = 0, s_nDlg = DLG_NONE, s_nDragY = -1;
	s_nStatusW = -1, s_nValidWas = -1, s_nClock = ~0u;
	memset (s_Drawn, 0, sizeof s_Drawn);
	if (!s_bHave) { memset (&s_V, 0, sizeof s_V); s_V.ntakes = -1; s_V.bits = 24; s_V.rate = 48000; }
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, Wd, H);
	s_Page = pg;
	const lv_font_t *mi = mi_font ();
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, Wd, 13);
		th_titlebar_set (s_TitleBar, "REC", "", 0);
		s_State = 0;
		s_RecKey = th_box (pg, 0, 15, 62, 46);
		s_ArmKey = th_key (pg, 64, 15, 46, 46, "ARM", TH_KEY, arm_cb, 0);
		s_StopKey = th_key (pg, 64, 15, 46, 46, "STOP", TH_KEY | TH_KEY_SMALL, stop_cb, 0);
		s_Clock = th_box (pg, 114, 15, 72, 22);
		s_Take = label_w (pg, 114, 38, 120 + dx / 2, TH->f_body, TH->ink, 0);
		s_Status = label_w (pg, 114, 51, 120 + dx / 2, mi, TH->dim, 0);
		for (int i = 0; i < 4; i++) s_Info[i] = label_w (pg, 237 + dx / 2, 16 + i * 11, 62 + dx / 2, mi, i == 0 ? TH->ink : TH->dim, 0);
		s_TakesKey = th_key (pg, 301 + dx, 15, 51, 46, "TAKES", TH_KEY | TH_KEY_SMALL, takes_cb, 0);
		for (int g = 0; g < 3; g++) s_Group[g] = th_label (pg, col_x (g) + 3, 63, mi, TH->faint, kGroup[g]);
	}
	else
	{
		s_TitleBar = 0;
		const int ww = 508 + dx;		// the display window; the keys at its right
		th_panel (pg, 8, 6, ww, 62, "", TH_PANEL_WINDOW);
		s_Clock = th_box (pg, 20, 6, 156, 62);
		s_Take = label_w (pg, 188, 12, ww - 188 - 112, &lv_font_montserrat_18, TH->win_ink, 0);
		s_State = label_w (pg, ww - 112, 15, 106, &lv_font_montserrat_14, TH->win_dim, 1);
		s_Status = label_w (pg, 188, 41, ww - 188 - 6, mi, TH->win_dim, 0);
		s_Info[0] = label_w (pg, 188 + 150, 41, ww - 188 - 150 - 6, mi, TH->win_dim, 1);
		s_Info[1] = s_Info[2] = s_Info[3] = 0;
		int kx = 8 + ww + 8;
		s_RecKey = th_box (pg, kx, 6, 100, 62);
		s_ArmKey = th_key (pg, kx + 106, 6, 80, 62, "ARM", TH_KEY, arm_cb, 0);
		s_StopKey = th_key (pg, kx + 106, 6, 80, 62, "STOP", TH_KEY, stop_cb, 0);
		th_key_font (s_StopKey, &lv_font_montserrat_16);
		s_TakesKey = th_key (pg, kx + 192, 6, Wd - 8 - (kx + 192), 62, "TAKES", TH_KEY, takes_cb, 0);
		th_key_font (s_ArmKey, &lv_font_montserrat_16);
		th_key_font (s_TakesKey, &lv_font_montserrat_16);
		for (int g = 0; g < 3; g++)
		{
			s_Group[g] = th_label (pg, col_x (g) + 2, 72, &lv_font_montserrat_12, TH->sub, kGroupC[g]);
			lv_obj_set_style_text_letter_space (s_Group[g], 2, 0);
		}
	}
	lv_obj_add_flag (s_RecKey, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_RecKey, rec_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_RecKey, rec_cb, LV_EVENT_ALL, 0);
	lv_obj_add_event_cb (s_Clock, clock_draw, LV_EVENT_DRAW_MAIN, 0);

	s_Grid = th_box (pg, 0, grid_y (), Wd, grid_h ());
	lv_obj_add_flag (s_Grid, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Grid, grid_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Grid, grid_cb, LV_EVENT_ALL, 0);

	// the takes: a header, a row a take (name, length, size, files, DEL)
	if (O)
	{
		s_TBox = th_box (pg, 0, 62, Wd, H - 62);
		static const char *const kH[4] = {"TAKE", "LENGTH", "SIZE", "FILES"};
		const int hx[4] = {3, 150 + dx / 3, 202 + dx / 2, 258 + 2 * dx / 3};
		for (int k = 0; k < 4; k++) s_THead[k] = th_label (s_TBox, hx[k], 1, mi, TH->faint, kH[k]);
		const int rh = pitch ();
		s_nRowH = rh;
		for (int i = 0; i < TROWS; i++)
		{
			int y = 10 + i * rh, ty = y + (rh - 11) / 2 + 1;
			s_THit[i] = th_box (s_TBox, 0, y, Wd - 50, rh);		// the row's tap (21 dots): play / pause
			lv_obj_add_flag (s_THit[i], LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_flag (s_THit[i], LV_OBJ_FLAG_PRESS_LOCK);		// a drag off the row still scrolls
			lv_obj_add_event_cb (s_THit[i], hit_draw, LV_EVENT_DRAW_MAIN, (void *) (intptr_t) i);
			lv_obj_add_event_cb (s_THit[i], hit_cb, LV_EVENT_ALL, (void *) (intptr_t) i);
			s_TName[i] = label_w (s_TBox, 16, ty, 130 + dx / 3, TH->f_body, TH->ink, 0);
			s_TLen[i] = label_w (s_TBox, hx[1], ty, 48, TH->f_body, TH->ink, 0);
			s_TSize[i] = label_w (s_TBox, hx[2], ty, 52, TH->f_body, TH->dim, 0);
			s_TFiles[i] = label_w (s_TBox, hx[3], ty, 30, TH->f_body, TH->dim, 0);
			s_TDel[i] = th_key (s_TBox, Wd - 48, y + 1, 48, rh - 2, "DEL", TH_KEY_DANGER | TH_KEY_SMALL, del_cb, (void *) (intptr_t) i);
			lv_obj_set_ext_click_area (s_TDel[i], 1);		// the row's whole pitch: 21 dots
		}
		s_TEmpty = label_w (s_TBox, 0, 10 + 3 * rh, Wd, TH->f_body, TH->dim, -1);
	}
	else
	{
		s_TBox = th_box (pg, 0, 72, Wd, H - 72);
		th_panel (s_TBox, 8, 0, Wd - 16, H - 76, "", TH_PANEL_WINDOW);
		static const char *const kH[4] = {"Take", "Length", "Size", "Files"};
		const int hx[4] = {28, 360 + dx / 3, 480 + dx / 2, 600 + 2 * dx / 3};
		for (int k = 0; k < 4; k++) s_THead[k] = th_label (s_TBox, hx[k], 10, &lv_font_montserrat_14, TH->win_dim, kH[k]);
		const int nr = (H - 76 - 40) / 44 < TROWS ? (H - 76 - 40) / 44 : TROWS, rh = (H - 76 - 40) / nr;	// 7 rows of 44 px, 8 of 54
		s_nRows = nr, s_nRowH = rh;
		for (int i = 0; i < TROWS; i++)
		{
			int y = 34 + i * rh, ty = y + (rh - 22) / 2;
			s_THit[i] = th_box (s_TBox, 14, y, Wd - 24 - 100 - 8 - 14, rh);	// the row's tap (44 px or more): play / pause
			lv_obj_add_flag (s_THit[i], LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_flag (s_THit[i], LV_OBJ_FLAG_PRESS_LOCK);		// a drag off the row still scrolls
			lv_obj_add_event_cb (s_THit[i], hit_draw, LV_EVENT_DRAW_MAIN, (void *) (intptr_t) i);
			lv_obj_add_event_cb (s_THit[i], hit_cb, LV_EVENT_ALL, (void *) (intptr_t) i);
			s_TName[i] = label_w (s_TBox, hx[0] + 18, ty, 302 + dx / 3, &lv_font_montserrat_18, TH->win_ink, 0);
			s_TLen[i] = label_w (s_TBox, hx[1], ty, 110, &lv_font_montserrat_18, TH->win_ink, 0);
			s_TSize[i] = label_w (s_TBox, hx[2], ty, 110, &lv_font_montserrat_18, TH->win_dim, 0);
			s_TFiles[i] = label_w (s_TBox, hx[3], ty, 60, &lv_font_montserrat_18, TH->win_dim, 0);
			s_TDel[i] = th_key (s_TBox, Wd - 24 - 100, y + 2, 100, rh - 4, "Delete", TH_KEY_DANGER, del_cb, (void *) (intptr_t) i);
			lv_obj_set_ext_click_area (s_TDel[i], 2);		// the row's pitch: 44 px
			th_key_font (s_TDel[i], &lv_font_montserrat_14);
		}
		s_TEmpty = label_w (s_TBox, 8, 34 + 3 * rh, Wd - 16, &lv_font_montserrat_18, TH->win_dim, -1);
	}
	lv_obj_add_flag (s_TBox, LV_OBJ_FLAG_HIDDEN);

	// recording off: one line over the grid
	s_Off = label_w (pg, 0, grid_y () + grid_h () / 2 - 12, Wd, O ? TH->f_body : &lv_font_montserrat_18, O ? TH->dim : TH->ink, -1);
	lv_obj_add_flag (s_Off, LV_OBJ_FLAG_HIDDEN);

	// the delete's confirmation: centred, its size kept on 1024x600, over a layer that takes every other tap (a cancel)
	s_Scrim = th_box (pg, 0, 0, Wd, H);
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Scrim, scrim_cb, LV_EVENT_CLICKED, 0);
	if (!O)
	{
		lv_obj_set_style_bg_color (s_Scrim, lv_color_hex (0x000000), 0);
		lv_obj_set_style_bg_opa (s_Scrim, LV_OPA_50, 0);
	}
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	if (O)
	{
		s_Dlg = th_box (pg, (Wd - 300) / 2, 66 + dy / 2, 300, 110);
		lv_obj_set_style_bg_color (s_Dlg, lv_color_hex (TH->bg), 0);
		lv_obj_set_style_bg_opa (s_Dlg, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Dlg, lv_color_hex (TH->ink), 0);
		lv_obj_set_style_border_width (s_Dlg, 1, 0);
		lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_CLICKABLE);		// taps stop here
		for (int i = 0; i < 3; i++) s_DlgText[i] = label_w (s_Dlg, 8, 8 + i * 16 + (i ? 4 : 0), 284, i ? mi : TH->f_body, i == 2 ? TH->accent_hi : TH->ink, 0);
		s_DlgNo = th_key (s_Dlg, 8, 70, 136, 32, "CANCEL", TH_KEY, dlg_cb, (void *) 0);
		s_DlgYes = th_key (s_Dlg, 156, 70, 136, 32, "DELETE", TH_KEY_DANGER | TH_KEY_FILL, dlg_cb, (void *) 1);
	}
	else
	{
		s_Dlg = th_box (pg, (Wd - 520) / 2, 100 + dy / 2, 520, 220);
		th_panel (s_Dlg, 0, 0, 520, 220, "", TH_PANEL_WINDOW);
		lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_CLICKABLE);
		for (int i = 0; i < 3; i++)
			s_DlgText[i] = label_w (s_Dlg, 24, 20 + i * 34 + (i ? 6 : 0), 472, i ? &lv_font_montserrat_16 : &lv_font_montserrat_22,
						i == 2 ? TH->warn : TH->win_ink, 0);
		s_DlgNo = th_key (s_Dlg, 24, 146, 224, 56, "Cancel", TH_KEY, dlg_cb, (void *) 0);
		s_DlgYes = th_key (s_Dlg, 272, 146, 224, 56, "Delete", TH_KEY_DANGER | TH_KEY_FILL, dlg_cb, (void *) 1);
		th_key_font (s_DlgNo, &lv_font_montserrat_20);
		th_key_font (s_DlgYes, &lv_font_montserrat_20);
	}
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
	if (s_bHave) { sidecar_rec_view_t v = s_V; s_nTakesGen = ~0u; sidecar_rec_set (&v); }
	view_update ();
}

void sidecar_rec_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
	else { lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN); dlg_show (-1); }
}

int sidecar_rec_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
int sidecar_rec_takes_shown (void) { return s_bTakes; }

void sidecar_rec_takes (int on)
{
	if (on && !s_bTakes) s_nTop = 0;	// the newest first
	s_bTakes = on != 0;
	if (s_Page) { view_update (); if (s_bTakes) takes_update (); }
}

// ---- the view
static void meters_advance (const sidecar_rec_view_t *v)
{
	uint32_t now = lv_tick_get ();
	float dt = s_nLastMs ? (float) (uint32_t) (now - s_nLastMs) / 1000.0f : 0.0f;
	if (dt > 0.5f) dt = 0.5f;
	s_nLastMs = now;
	float fall = FALL_DB_S * dt;
	for (int id = 0; id < SRV_CH; id++)
		for (int s = 0; s < 2; s++)
		{
			unsigned p = v->peak[id][s];
			float db = p ? 20.0f * log10f ((float) p / (float) SRV_FS) : -120.0f;
			float d = s_Disp[id][s] - fall;
			s_Disp[id][s] = db > d ? db : d < -120.0f ? -120.0f : d;
			if (db >= s_Hold[id][s]) { s_Hold[id][s] = db; s_HoldAt[id][s] = now; }
			else if (now - s_HoldAt[id][s] > HOLD_MS)
			{
				float h = s_Hold[id][s] - fall;
				s_Hold[id][s] = h > s_Disp[id][s] ? h : s_Disp[id][s];
			}
			if (p >= SRV_FS - 8) s_ClipAt[id] = now ? now : 1;	// -0.00 dBFS: a clip, held
		}
}

void sidecar_rec_set (const sidecar_rec_view_t *v)
{
	int was = s_V.state;
	unsigned mask = s_V.mask;
	s_V = *v;
	s_bHave = 1;
	if (!s_Page) return;
	meters_advance (v);
	int O = oled (), st = v->state;
	char b[80], r[64];
	// the title / state
	static const char *const kSt[4] = {"IDLE", "ARMED: PLAY STARTS", "RECORDING", "SAVING"};
	static const char *const kStC[4] = {"Idle", "Armed: PLAY", "Recording", "Saving..."};	// (the state's box: 106 px)
	const char *sn = st >= 0 && st < 4 ? (O ? kSt : kStC)[st] : "";
	if (st == SRV_REC && v->arm_mode) sn = O ? "RECORDING: STOP ENDS" : "Rec: STOP ends";
	if (!v->valid) sn = O ? "OFF" : "Off";
	if (O)
	{
		th_titlebar_set (s_TitleBar, "REC", sn, st == SRV_REC);	// T15: recording - the whole title bar in the alert look
		int blink = st == SRV_REC && (v->secs & 1) == 0;
		th_titlebar_led (s_TitleBar, st == SRV_REC ? (blink ? LED_PLAY : 0) : st == SRV_ARMED ? LED_HELD : 0);
	}
	else
	{
		th_label_set (s_State, sn);
		th_label_color (s_State, st == SRV_REC ? TH->bad : st == SRV_ARMED ? TH->warn : TH->win_dim);
	}
	// the take, the format, the card
	if (O) lv_snprintf (b, sizeof b, "%s", v->take[0] ? v->take : "NO TAKE YET");
	else lv_snprintf (b, sizeof b, "REC   %s", v->take[0] ? v->take : "no take yet");
	th_label_fit (s_Take, b);
	th_label_color (s_Take, st == SRV_REC ? (O ? TH->accent_hi : 0xFFFFFF) : v->take[0] ? (O ? TH->ink : TH->win_ink) : (O ? TH->dim : TH->win_dim));
	unsigned files = 0;
	for (int id = 0; id < SRV_CH; id++) files += v->mask >> id & 1;
	unsigned left = v->need_kbps ? (unsigned) ((unsigned long long) v->free_mb * 1024 / v->need_kbps) : 0;
	char fr[24], lf[20], dv[8] = "";
	int drv = v->vol[0] && strcmp (v->vol, "SD") != 0;	// a drive: named before its free space ("USB 57 GB")
	if (drv) lv_snprintf (dv, sizeof dv, "%s ", v->vol);
	if (v->free_mb >= 10240) lv_snprintf (fr, sizeof fr, "%s%u GB", dv, v->free_mb / 1024);
	else if (v->free_mb >= 1024) lv_snprintf (fr, sizeof fr, "%s%u.%u GB", dv, v->free_mb / 1024, v->free_mb % 1024 * 10 / 1024);
	else lv_snprintf (fr, sizeof fr, "%s%u MB", dv, v->free_mb);
	if (!files) lv_snprintf (lf, sizeof lf, "-");
	else if (left >= 3600) lv_snprintf (lf, sizeof lf, O ? "%uH%02uM" : drv ? "%u:%02u h" : "%u h %02u min left", left / 3600, left / 60 % 60);
	else lv_snprintf (lf, sizeof lf, O ? "%uM%02uS" : drv ? "%u:%02u min" : "%u min %02u s left", left / 60, left % 60);	// (a drive: shorter, its name in front)
	int known = !(drv && (v->vol_missing || v->free_unknown));	// the free space and the time left mean something
	if (drv && v->vol_missing) lv_snprintf (fr, sizeof fr, O ? "NO %s" : "%snot mounted", O ? v->vol : dv), lv_snprintf (lf, sizeof lf, "-");
	else if (drv && v->free_unknown) lv_snprintf (fr, sizeof fr, "%s? free", dv), lv_snprintf (lf, sizeof lf, "-");	// (stor=free:VOL)
	const char *msg = v->error[0] ? v->error : "";
	if (O)
	{
		lv_snprintf (b, sizeof b, "%uK %u-BIT", v->rate / 1000, v->bits);
		th_label_set (s_Info[0], b);
		lv_snprintf (b, sizeof b, "%u FILE%s", files, files == 1 ? "" : "S");
		th_label_set (s_Info[1], b);
		if (drv) lv_snprintf (b, sizeof b, "%s", v->total_mb || !known ? fr : dv);	// (no room for FREE beside the drive)
		else lv_snprintf (b, sizeof b, "%s FREE", v->total_mb ? fr : "-");
		th_label_set (s_Info[2], b);
		lv_snprintf (b, sizeof b, "%s LEFT", v->total_mb && known ? lf : "-");
		th_label_set (s_Info[3], b);
		th_label_color (s_Info[3], v->total_mb && known && files && left < 600 ? TH->accent_hi : TH->dim);
		th_label_color (s_Info[2], drv && v->vol_missing ? TH->accent_hi : TH->dim);
		if (msg[0]) lv_snprintf (r, sizeof r, "%s", msg);
		else if (v->drops) lv_snprintf (r, sizeof r, "CARD BEHIND: %u DROPPED", v->drops);
		else if (st == SRV_ARMED) lv_snprintf (r, sizeof r, "RYTM PLAY STARTS IT");
		else if (st == SRV_REC) lv_snprintf (r, sizeof r, v->arm_mode ? "RYTM STOP ENDS IT" : "TAP STOP TO END");
		else if (st == SRV_SAVING) lv_snprintf (r, sizeof r, "CLOSING THE FILES");
		else lv_snprintf (r, sizeof r, "TAP: CHANNEL ON/OFF");
		for (char *q = r; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
		th_label_set (s_Status, r);
		th_label_color (s_Status, msg[0] || v->drops ? TH->accent_hi : TH->dim);
	}
	else
	{
		int warn = msg[0] || v->drops;
		if (msg[0]) lv_snprintf (r, sizeof r, "%s", msg);
		else if (v->drops) lv_snprintf (r, sizeof r, "The card fell behind: %u blocks written as silence", v->drops);
		else lv_snprintf (r, sizeof r, "%u kHz  %u-bit  %u file%s", v->rate / 1000, v->bits, files, files == 1 ? "" : "s");
		th_label_set (s_Status, r);
		th_label_color (s_Status, warn ? TH->warn : TH->win_dim);
		int full = lv_obj_get_width (s_Take) + 112 - 6, sw = warn ? full : th_text_width (mi_font (), r) + 8;
		if (sw > full) sw = full;
		if (sw != s_nStatusW)				// (only a change moves them)
		{
			s_nStatusW = sw;
			lv_obj_set_width (s_Status, sw);
			lv_obj_set_x (s_Info[0], 188 + sw);
			lv_obj_set_width (s_Info[0], full - sw > 0 ? full - sw : 1);
		}
		if (!known) lv_snprintf (b, sizeof b, "%s", fr);
		else if (v->total_mb) lv_snprintf (b, sizeof b, "%s  %s", fr, lf);
		else lv_snprintf (b, sizeof b, "%s", drv ? dv : "-");
		th_label_set (s_Info[0], warn ? "" : b);
		th_label_color (s_Info[0], (drv && v->vol_missing) || (v->total_mb && known && files && left < 600) ? TH->warn : TH->win_dim);
	}
	// the keys
	th_key_text (s_ArmKey, st == SRV_ARMED ? "DISARM" : "ARM");
	th_key_led (s_ArmKey, st == SRV_ARMED ? LED_HELD : st == SRV_REC && v->arm_mode ? (O ? LED_PLAY : LED_REC) : 0);
	th_key_enable (s_ArmKey, v->valid && !busy ());
	if (st != was || st == SRV_ARMED || v->valid != s_nValidWas) lv_obj_invalidate (s_RecKey);	// (the armed blink)
	unsigned ck = (st == SRV_ARMED ? 0 : v->secs) * 8u + (unsigned) st + (v->take[0] ? 4u : 0u);
	if (ck != s_nClock) { s_nClock = ck; lv_obj_invalidate (s_Clock); }	// once a second at most
	s_nValidWas = v->valid;
	// recording off
	if (!v->valid)
	{
		th_label_set (s_Off, O ? "RECORDING IS OFF (CMDLINE RECBUF=0, OR NO CARD)" : "Recording is off (cmdline recbuf=0, or no card)");
		lv_obj_remove_flag (s_Off, LV_OBJ_FLAG_HIDDEN);
	}
	else lv_obj_add_flag (s_Off, LV_OBJ_FLAG_HIDDEN);
	if (s_nDlg == DLG_WARN && st != SRV_IDLE) dlg_show (-1);	// started meanwhile (the web page, the Rytm's PLAY)
	// the takes
	if (v->takes_gen != s_nTakesGen || st != was || v->pl_state != s_nPlSt || (v->pl_state && s_nPlRow != pl_row ()))
	{
		s_nTakesGen = v->takes_gen;
		takes_update ();
	}
	else if (s_bTakes && s_nPlRow >= 0 && lv_tick_get () - s_nPlAt >= 100)		// the playing row: <= 10 a second
	{
		char tb[32];
		s_nPlAt = lv_tick_get ();
		pl_time (tb, sizeof tb);
		th_label_set (s_TLen[s_nPlRow], tb);					// (redrawn when the second changes)
		int px = bar_px (bar_w (s_THit[s_nPlRow]));
		if (px != s_nPlPx) { s_nPlPx = px; bar_inval (s_nPlRow); }		// the bar alone
	}
	// the cells: only those whose picture changed (s_Drawn: what each cell was last asked to show)
	int all = st != was || v->mask != mask;
	if (all) lv_obj_invalidate (s_Grid);
	for (int k = 0; k < CELLS; k++)
	{
		cell_t cs;
		cell_state (k, &cs);
		const cell_t *d = &s_Drawn[k];
		if (!all && !s_bTakes && memcmp (&cs, d, sizeof cs))		// the meter alone, unless the cell's look changed
		{
			if (cs.armed != d->armed || cs.press != d->press || cs.en != d->en) cell_inval (k);
			else meter_inval (k);
		}
		s_Drawn[k] = cs;
	}
}
