// pi/ui/sidecar_scope.c - see sidecar_scope.h. Three code-drawn areas and a row of keys:
//   the trace   min / max columns over a dotted graticule (8 time divisions, the middle and half scale), the source,
//               the gain and the selected channel's peak in its corners; redrawn only when a new trace comes
//   the meters  eleven channels (MAIN as L / R bars) as LED ladders, each one's strip redrawn alone when its lit
//               segments or its hold moved; a tap on a meter shows that channel (TRACK follows the Rytm again)
//   the keys    FREEZE, the timebase down / up around its value, GAIN (AUTO, x1 .. x64)
// Every mark goes through th_fill / th_text / th_paint_key, so the OLED theme keeps its dots: the trace in the light
// print, the graticule dotted, the meters dim grey / white / teal by level (no red); CLASSIC draws the Rytm's display
// window with the trace in its white, the meters as recessed LED ladders (green, amber, red) in rubber keys.
#include "sidecar_scope.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"	// lv_area_intersect, l->_clip_area: a meter's strip redrawn alone
#include <math.h>
#include <string.h>

#define DB_FLOOR	(-60.0f)
#define FALL_DB_S	24.0f
#define HOLD_MS		1500u
#define CLIP_MS		3000u
#define NCOL		11		// meter columns: MAIN, the 8 stems, IN L, IN R (the 12th place: TRACK)
#define AUTO_UP		20		// AUTO raises its gain after this many traces (~1.3 s at 15 a second), as page_scope.cpp

static const char *const kTb[SSC_TBS] = {"43MS", "85MS", "171MS", "341MS", "683MS", "1.4S", "2.7S"};
static const char *const kTbC[SSC_TBS] = {"43 ms", "85 ms", "171 ms", "341 ms", "683 ms", "1.4 s", "2.7 s"};
static const unsigned char kTbEntries[SSC_TBS] = {1, 2, 4, 8, 16, 32, 64};
static const char *const kDefName[SSC_SOURCES] = {"MAIN", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
static const char *const kTrack[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Info, *s_Area, *s_Meters, *s_TrackKey, *s_Freeze, *s_TbDn, *s_TbUp, *s_TbVal, *s_Gain;
static sidecar_scope_view_t s_V;			// the last view (its trace held while frozen)
static int s_bHave, s_nSrc = -1, s_nTb = 1, s_nGainSel, s_bFrozen, s_nPress = -1;
static int s_nAutoG = 64, s_nAutoUp, s_nShownG = 1;	// AUTO: the gain shown, how long a higher one has been wanted
static unsigned s_nTraceGen = ~0u;
static uint32_t s_nLastMs;
static float s_Disp[SSC_CH], s_Hold[SSC_CH];
static uint32_t s_HoldAt[SSC_CH], s_ClipAt[SSC_CH];
typedef struct { signed char lit[2], hold[2]; unsigned char clip, sel, press; } col_t;
static col_t s_Drawn[NCOL];

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
const char *sidecar_scope_tb_name (int tb) { return kTb[tb < 0 ? 0 : tb >= SSC_TBS ? SSC_TBS - 1 : tb]; }
int sidecar_scope_tb_entries (int tb) { return kTbEntries[tb < 0 ? 0 : tb >= SSC_TBS ? SSC_TBS - 1 : tb]; }
int sidecar_scope_source (void) { return s_nSrc; }
int sidecar_scope_timebase (void) { return s_nTb; }
int sidecar_scope_frozen (void) { return s_bFrozen; }
int sidecar_scope_gain (void) { return s_nGainSel; }
static const char *src_name (int s)
{
	if (s < 0 || s >= SSC_SOURCES) return "-";
	return s_bHave && s_V.name[s][0] ? s_V.name[s] : kDefName[s];
}

// ---- geometry: the 800x480 layouts plus what the screen adds (TH->dx, dy): the trace gets two thirds of the height,
// the meters the rest, every column wider
static int PW (void) { return oled () ? TH->w - 48 : TH->w; }
static int area_h (void) { return oled () ? 133 + TH->dy * 2 / 3 : 196 + TH->dy * 2 / 3; }
static int strip_y (void) { return oled () ? 18 + area_h () : 254 + TH->dy * 2 / 3; }
static int strip_h (void) { return oled () ? 63 + TH->dy / 3 : 108 + TH->dy / 3; }
static int strip_x (void) { return oled () ? 0 : 8; }
static int strip_w (void) { return oled () ? PW () : PW () - 16; }
static int col_gap (void) { return oled () ? 2 : 8; }
static int col_w (void) { return (strip_w () + col_gap ()) / (NCOL + 1) - col_gap (); }
static int col_x (int k) { return k * (col_w () + col_gap ()); }		// in the strip
static int lab_h (void) { return oled () ? 9 : 20; }				// the label under a ladder
// a column's ladder, in the strip's coordinates: x range, top, height (segments: 2 dots / 3 px tall, a gap of 1)
static void ladder (int k, int *x0, int *x1, int *y0, int *h)
{
	int x = col_x (k), w = col_w ();
	if (oled ()) { *x0 = x + 3, *x1 = x + w - 4, *y0 = 5, *h = strip_h () - lab_h () - 7; }
	else { *x0 = x + 10, *x1 = x + w - 11, *y0 = 12, *h = strip_h () - lab_h () - 22; }
}
static int seg_pitch (void) { return oled () ? 3 : 4; }
static int nseg (void) { int x0, x1, y0, h; ladder (0, &x0, &x1, &y0, &h); return h / seg_pitch (); }
static int seg_of (float db, int n)
{
	if (db <= DB_FLOOR) return 0;
	int k = (int) ((db - DB_FLOOR) / -DB_FLOOR * n + 0.5f);
	return k < 0 ? 0 : k > n ? n : k;
}
static int col_src (int k) { return k; }					// column k shows source k
static int col_ch (int k, int side) { return k == 0 ? side : k <= 8 ? k + 1 : k + 1; }	// MAIN L / R, the stems, IN L / R
static int col_stereo (int k) { return k == 0; }
static int src_now (void) { return s_bHave ? s_V.src : s_nSrc < 0 ? 0 : s_nSrc; }

// ---- the trace
// one fill a line (OLED: the dark checkerboard DOT_SHADE, every other dot of a line lit dim; the middle and the frame
// DOT_DOTTED, every other dot light), so the graticule costs a dozen fills, not a fill a dot
static void graticule (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	int O = oled ();
	uint32_t g = O ? DOT_SHADE : th_mix (TH->win, TH->win_dim, 50), mid = O ? DOT_DOTTED : th_mix (TH->win, TH->win_dim, 95);
	for (int d = 1; d < 8; d++) th_fill (l, c, d * w / 8, 1, d * w / 8, h - 2, g, LV_OPA_COVER, 0);
	for (int k = 1; k <= 3; k++) th_fill (l, c, 1, k * h / 4, w - 2, k * h / 4, k == 2 ? mid : g, LV_OPA_COVER, 0);
	if (O)							// the frame: dotted, as a panel's
	{
		th_fill (l, c, 0, 0, w - 1, 0, DOT_DOTTED, LV_OPA_COVER, 0);
		th_fill (l, c, 0, h - 1, w - 1, h - 1, DOT_DOTTED, LV_OPA_COVER, 0);
		th_fill (l, c, 0, 0, 0, h - 1, DOT_DOTTED, LV_OPA_COVER, 0);
		th_fill (l, c, w - 1, 0, w - 1, h - 1, DOT_DOTTED, LV_OPA_COVER, 0);
	}
}

static int peak_db10 (int src)		// the selected channel's level now, dB x 10 (louder side of a stereo pair)
{
	int k = src < 0 ? 0 : src >= NCOL ? NCOL - 1 : src;
	float d = s_Disp[col_ch (k, 0)];
	if (col_stereo (k) && s_Disp[col_ch (k, 1)] > d) d = s_Disp[col_ch (k, 1)];
	return (int) (d * 10.0f - 0.5f);
}

static void area_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	if (!O) th_fill (l, &c, 0, 0, w - 1, h - 1, TH->win, LV_OPA_COVER, 0);
	graticule (l, &c, w, h);
	const lv_font_t *fm = O ? TH->f_micro : &lv_font_montserrat_14;
	int fh = lv_font_get_line_height (fm), pad = O ? 3 : 8;
	if (!s_bHave || !s_V.valid)
	{
		th_text (l, &c, 0, h / 2 - fh - 2, w, O ? TH->f_body : &lv_font_montserrat_18, c_dim (), LV_TEXT_ALIGN_CENTER,
			 O ? "NO OVERBRIDGE AUDIO YET" : "No Overbridge audio yet");
		th_text (l, &c, 0, h / 2 + 4, w, fm, c_dim (), LV_TEXT_ALIGN_CENTER,
			 O ? "THE RYTM'S USB AUDIO: THE TRACKS, MAIN AND IN" : "the Rytm's USB audio: its tracks, MAIN and IN");
		return;
	}
	const sidecar_scope_view_t *v = &s_V;
	int g = s_nShownG, mid = h / 2, amp = h / 2 - 2, n = v->ncols;
	uint32_t ink = s_bFrozen ? (O ? TH->accent_hi : 0xBFE7F5) : c_ink ();
	int pa = -1, pb = -1, step = O ? 1 : 2;			// CLASSIC: a 2 px column every 2 px
	int xs = l->_clip_area.x1 - c.x1 - 2 * step, xe = l->_clip_area.x2 - c.x1 + step;	// only the columns redrawn
	xs = xs < 1 ? 1 : xs - (xs - 1) % step;
	for (int x = xs; n > 0 && x < w - 1 && x <= xe; x += step)
	{
		int i = (x - 1) * SSC_COLS / (w - 2);
		if (i >= n) break;
		if (v->cursor >= 0 && (i == v->cursor || i == (v->cursor + 1) % SSC_COLS)) { pa = -1; continue; }	// the sweep's gap
		long hi = (long) v->hi[i] * g, lo = (long) v->lo[i] * g;
		int a = mid - (int) (hi * amp / 32768), b = mid - (int) (lo * amp / 32768);
		if (a < 1) a = 1;
		if (b > h - 2) b = h - 2;
		if (b < a) { int t = a; a = b; b = t; }
		if (pa >= 0) { if (a > pb) a = pb; if (b < pa) b = pa; }	// joined to the column before: no gaps on a slope
		th_fill (l, &c, x, a, x + step - 1, b, ink, LV_OPA_COVER, 0);
		pa = mid - (int) (hi * amp / 32768), pb = mid - (int) (lo * amp / 32768);
		if (pa > pb) { int t = pa; pa = pb; pb = t; }
	}
	// the corners: the source (TRACK: the Rytm's), the gain; the timebase a division; the peak; FROZEN
	char b[40];
	int s = v->src;
	if (s_nSrc < 0) lv_snprintf (b, sizeof b, O ? "TRACK %s" : "Track %s", v->track >= 0 && v->track < 12 ? kTrack[v->track] : "-");
	else lv_snprintf (b, sizeof b, "%s", src_name (s));
	th_text (l, &c, pad, pad - 1, w / 2, O ? TH->f_body : &lv_font_montserrat_16, c_ink (), LV_TEXT_ALIGN_LEFT, b);
	if (s_nGainSel) lv_snprintf (b, sizeof b, "X%d", g); else lv_snprintf (b, sizeof b, O ? "AUTO X%d" : "AUTO x%d", g);
	th_text (l, &c, w / 2, pad, w / 2 - pad, fm, c_dim (), LV_TEXT_ALIGN_RIGHT, b);
	int pk = peak_db10 (s);
	if (pk <= DB_FLOOR * 10) lv_snprintf (b, sizeof b, O ? "PEAK -INF" : "peak -inf");
	else lv_snprintf (b, sizeof b, O ? "PEAK %d.%d DB" : "peak %d.%d dB", pk / 10, (pk < 0 ? -pk : pk) % 10);
	th_text (l, &c, w / 2, h - fh - pad + 1, w / 2 - pad, fm, pk > -30 ? (O ? TH->accent : TH->ok) : c_dim (), LV_TEXT_ALIGN_RIGHT, b);
	int per = (int) ((double) kTbEntries[v->tb] * 8 * SSC_COLS * 1000 / 48000 / 8 * 10);	// a division, ms x 10
	if (per >= 1000) lv_snprintf (b, sizeof b, O ? "%d MS/DIV" : "%d ms/div", per / 10);
	else lv_snprintf (b, sizeof b, O ? "%d.%d MS/DIV" : "%d.%d ms/div", per / 10, per % 10);
	th_text (l, &c, pad, h - fh - pad + 1, w / 2, fm, c_dim (), LV_TEXT_ALIGN_LEFT, b);
	if (s_bFrozen) th_text (l, &c, 0, pad, w, fm, O ? TH->accent_hi : 0xBFE7F5, LV_TEXT_ALIGN_CENTER, O ? "FROZEN" : "FROZEN");
	else if (v->tb < SSC_TRIG_TBS) th_text (l, &c, 0, pad, w, fm, c_dim (), LV_TEXT_ALIGN_CENTER, O ? "TRIG" : "triggered");
}

// the area's columns from column a to b (the sweep's new ones, wrapping), the gap after b, the peak's corner
static void area_inval_span (const lv_area_t *c, int w, int i0, int i1)
{
	lv_area_t a = {c->x1 + 1 + i0 * (w - 2) / SSC_COLS - 3, c->y1, c->x1 + 1 + (i1 + 1) * (w - 2) / SSC_COLS + 3, c->y2};
	lv_obj_invalidate_area (s_Area, &a);
}
static void area_inval_sweep (int from, int to)
{
	lv_area_t c;
	lv_obj_get_coords (s_Area, &c);
	int w = lv_area_get_width (&c), gap = (to + 2) % SSC_COLS;
	if (from <= gap) area_inval_span (&c, w, from, gap);
	else { area_inval_span (&c, w, from, SSC_COLS - 1); area_inval_span (&c, w, 0, gap); }
	int fh = lv_font_get_line_height (oled () ? TH->f_micro : &lv_font_montserrat_14), pad = oled () ? 3 : 8;
	lv_area_t pk = {c.x1 + w / 2, c.y2 - fh - pad, c.x2, c.y2};
	lv_obj_invalidate_area (s_Area, &pk);
}

// ---- the meters
static void col_state (int k, col_t *cs)
{
	int n = nseg ();
	uint32_t now = lv_tick_get ();
	memset (cs, 0, sizeof *cs);
	for (int sd = 0; sd < (col_stereo (k) ? 2 : 1); sd++)
	{
		int ch = col_ch (k, sd);
		cs->lit[sd] = (signed char) seg_of (s_Disp[ch], n);
		cs->hold[sd] = (signed char) seg_of (s_Hold[ch], n);
		if (s_ClipAt[ch] && now - s_ClipAt[ch] < CLIP_MS) cs->clip = 1;
	}
	cs->sel = s_nSrc == col_src (k) || (s_nSrc < 0 && src_now () == col_src (k));
	cs->press = s_nPress == k;
}

static void bar (lv_layer_t *l, const lv_area_t *c, int x0, int x1, int ybot, int n, int lit, int hold)
{
	int O = oled (), sh = O ? 2 : 3, sp = seg_pitch ();
	for (int i = 0; i < n; i++)
	{
		float db = DB_FLOOR + (i + 1) * -DB_FLOOR / n;
		uint32_t col;
		if (i < lit || i == hold - 1)
		{
			if (O) col = db > -6.0f ? TH->accent : db > -18.0f || i == hold - 1 ? TH->ink : TH->dim;
			else
			{
				col = db > -3.0f ? 0xFF4A3D : db > -12.0f ? 0xE8B040 : 0x34C85A;
				if (i == hold - 1 && i >= lit) col = 0xF0F0F0;
			}
		}
		else col = O ? 0x262626 : 0x1E2022;
		int y = ybot - i * sp;
		th_fill (l, c, x0, y - sh + 1, x1, y, col, LV_OPA_COVER, 0);
	}
}

// what a column's meter changes (its ladders, the clip lamp), in the strip's coordinates
static lv_area_t col_dyn (int k)
{
	int x0, x1, y0, h;
	ladder (k, &x0, &x1, &y0, &h);
	lv_area_t a = {x0, y0 - (oled () ? 3 : 6), x1, y0 + h - 1};
	return a;
}

static void meters_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), n = nseg (), cw = col_w ();
	const lv_area_t *clip = &l->_clip_area;
	const lv_font_t *fl = O ? TH->f_micro : &lv_font_montserrat_14;
	for (int k = 0; k < NCOL; k++)
	{
		int x = col_x (k), x0, x1, y0, h;
		lv_area_t ca = {c.x1 + x - 4, c.y1, c.x1 + x + cw + 3, c.y2}, tmp;
		if (!lv_area_intersect (&tmp, &ca, clip)) continue;	// (a meter's redraw: the other columns untouched)
		col_t cs;
		col_state (k, &cs);
		s_Drawn[k] = cs;
		ladder (k, &x0, &x1, &y0, &h);
		lv_area_t md = col_dyn (k);
		lv_area_move (&md, c.x1, c.y1);
		int only = lv_area_is_in (clip, &md, 0);
		if (only) th_fill (l, &c, md.x1 - c.x1, md.y1 - c.y1, md.x2 - c.x1, md.y2 - c.y1, O ? (cs.press ? DOT_DITHER : TH->bg) : 0x08090A, LV_OPA_COVER, 0);
		else if (O)
		{
			if (cs.press) th_fill (l, &c, x, 0, x + cw - 1, strip_h () - 1, DOT_DITHER, LV_OPA_COVER, 0);
			th_frame (l, &c, x, 0, x + cw - 1, strip_h () - 1, cs.sel ? TH->ink : DOT_DOTTED, 1, 0);
			int ly = strip_h () - lab_h ();
			if (cs.sel) th_fill (l, &c, x + 1, ly - 1, x + cw - 2, strip_h () - 2, TH->sel_bg, LV_OPA_COVER, 0);
			char sn[8];				// the first track's name in a narrow column: "RS/CP" -> "RS"
			lv_snprintf (sn, sizeof sn, "%s", src_name (k));
			char *sl = strchr (sn, '/');
			if (sl && cw < 34) *sl = 0;
			th_text (l, &c, x, ly, cw, fl, cs.sel ? TH->sel_ink : cs.press ? 0x000000 : TH->dim, LV_TEXT_ALIGN_CENTER, sn);
		}
		else
		{
			lv_area_t a = {c.x1 + x, c.y1, c.x1 + x + cw - 1, c.y2 - 3};
			th_paint_key (l, &a, "", 0, fl, TH_KEY, cs.sel, cs.press, 0, 0);
			th_fill (l, &c, x0 - 4, y0 - 8, x1 + 4, y0 + h + 2, 0x08090A, LV_OPA_COVER, 3);	// the meter's window
			th_frame (l, &c, x0 - 4, y0 - 8, x1 + 4, y0 + h + 2, 0x3A3C40, 1, 3);
			th_text (l, &c, x, y0 + h + 6, cw, fl, cs.sel ? 0xF0F0F0 : 0xB8BBC0, LV_TEXT_ALIGN_CENTER, src_name (k));
		}
		// the ladders: MAIN two (L, R), the others one; the clip lamp on top
		int bw = x1 - x0 + 1, yb = y0 + h - 1;
		if (col_stereo (k))
		{
			int hw = (bw - (O ? 1 : 2)) / 2;
			bar (l, &c, x0, x0 + hw - 1, yb, n, cs.lit[0], cs.hold[0]);
			bar (l, &c, x1 - hw + 1, x1, yb, n, cs.lit[1], cs.hold[1]);
		}
		else
		{
			int m = O ? (bw > 9 ? (bw - 9) / 2 : 0) : (bw > 18 ? (bw - 18) / 2 : 0);
			bar (l, &c, x0 + m, x1 - m, yb, n, cs.lit[0], cs.hold[0]);
		}
		if (O) th_fill (l, &c, x0, y0 - 3, x1, y0 - 2, cs.clip ? TH->accent_hi : 0x262626, LV_OPA_COVER, 0);
		else th_fill (l, &c, x0, y0 - 6, x1, y0 - 3, cs.clip ? 0xFF3B30 : 0x3A2A2A, LV_OPA_COVER, 1);
	}
}

static void col_inval (int k, int whole)
{
	lv_area_t c, a;
	lv_obj_get_coords (s_Meters, &c);
	if (whole) { int x = col_x (k); a = (lv_area_t) {x - 4, 0, x + col_w () + 3, strip_h () - 1}; }
	else a = col_dyn (k);
	lv_area_move (&a, c.x1, c.y1);
	lv_obj_invalidate_area (s_Meters, &a);
}

static int col_at (lv_point_t p)
{
	lv_area_t c;
	lv_obj_get_coords (s_Meters, &c);
	int x = p.x - c.x1, k = x / (col_w () + col_gap ());
	return k >= 0 && k < NCOL ? k : -1;
}

static void keys_update (void);
static void meters_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	if (code == LV_EVENT_PRESSED)
	{
		s_nPress = col_at (p);
		if (s_nPress >= 0) col_inval (s_nPress, 1);
	}
	else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		int k = s_nPress;
		s_nPress = -1;
		if (k < 0) return;
		col_inval (k, 1);
		if (code == LV_EVENT_RELEASED && col_at (p) == k)
		{
			int was = src_now ();
			s_nSrc = col_src (k);
			if (s_bHave) s_V.src = s_nSrc;		// shown at once; the provider's next trace is of it
			col_inval (was, 1);
			col_inval (k, 1);
			keys_update ();
			lv_obj_invalidate (s_Area);
		}
	}
}

// ---- the keys
static void keys_update (void)
{
	char b[40], r[64];
	th_key_check (s_Freeze, s_bFrozen);
	th_key_check (s_TrackKey, s_nSrc < 0);
	th_key_enable (s_TbDn, s_nTb > 0);
	th_key_enable (s_TbUp, s_nTb < SSC_TBS - 1);
	th_label_set (s_TbVal, oled () ? kTb[s_nTb] : kTbC[s_nTb]);
	if (s_nGainSel) lv_snprintf (b, sizeof b, oled () ? "GAIN X%d" : "Gain x%d", 1 << (s_nGainSel - 1));
	else lv_snprintf (b, sizeof b, oled () ? "GAIN AUTO" : "Gain AUTO");
	th_key_text (s_Gain, b);
	int tr = s_bHave ? s_V.track : -1;
	th_key_sub (s_TrackKey, tr >= 0 && tr < 12 ? kTrack[tr] : "-");
	const char *sn = s_nSrc < 0 ? (tr >= 0 && tr < 12 ? kTrack[tr] : "-") : src_name (s_nSrc);
	if (oled ())
	{
		lv_snprintf (r, sizeof r, "%s%s  %s", s_bFrozen ? "FROZEN  " : "", sn, kTb[s_nTb]);
		th_titlebar_set (s_TitleBar, "SCOPE", r, 0);
	}
	else
	{
		lv_snprintf (r, sizeof r, "SCOPE   %s%s", s_nSrc < 0 ? "Track " : "", sn);
		th_label_set (s_Title, r);
		lv_snprintf (r, sizeof r, "%s a screen%s", kTbC[s_nTb], s_bFrozen ? "   FROZEN" : "");
		th_label_set (s_Info, r);
	}
}

static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	if (k == 0) s_bFrozen = !s_bFrozen;
	else if (k == 1 && s_nTb > 0) s_nTb--;
	else if (k == 2 && s_nTb < SSC_TBS - 1) s_nTb++;
	else if (k == 3) s_nGainSel = (s_nGainSel + 1) % 8, s_nAutoG = 64, s_nAutoUp = 0;
	else if (k == 4)						// TRACK: follow the Rytm's selected track
	{
		int was = src_now ();
		s_nSrc = -1;
		col_inval (was, 1);
	}
	keys_update ();
	lv_obj_invalidate (s_Area);
}

void sidecar_scope_create (void *parent, int y0)
{
	int O = oled ();
	const int pw = PW (), dx = TH->dx;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, O ? TH->h : 428 + TH->dy);
	s_Page = pg;
	memset (s_Drawn, 0, sizeof s_Drawn);
	s_nTraceGen = ~0u;
	const int sy = strip_y (), sh = strip_h ();
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, 13);
		s_Title = s_Info = 0;
		s_Area = th_box (pg, 0, 15, pw, area_h ());
		const int ky = TH->h - 22, q = dx / 4;			// the keys: 22 dots (44 px) tall
		s_Freeze = th_key (pg, 0, ky, 76 + q, 22, "FREEZE", TH_KEY_FUNC, key_cb, (void *) 0);
		s_TbDn = th_key (pg, 80 + q, ky, 40, 22, LV_SYMBOL_LEFT, TH_KEY, key_cb, (void *) 1);
		lv_obj_t *box = th_panel (pg, 123 + q, ky, 76 + q, 22, "", TH_PANEL_BARE);
		s_TbVal = th_label (box, 0, 6, TH->f_body, TH->ink, "");
		lv_obj_set_width (s_TbVal, 76 + q);
		lv_obj_set_style_text_align (s_TbVal, LV_TEXT_ALIGN_CENTER, 0);
		s_TbUp = th_key (pg, 202 + 2 * q, ky, 40, 22, LV_SYMBOL_RIGHT, TH_KEY, key_cb, (void *) 2);
		s_Gain = th_key (pg, 246 + 2 * q, ky, pw - 246 - 2 * q, 22, "GAIN AUTO", TH_KEY, key_cb, (void *) 3);
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, 8, 6, pw - 16, area_h () + 44, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "SCOPE");
		s_Info = th_label (pg, pw / 2, 16, &lv_font_montserrat_14, TH->win_dim, "");
		lv_obj_set_width (s_Info, pw / 2 - 24);
		lv_obj_set_style_text_align (s_Info, LV_TEXT_ALIGN_RIGHT, 0);
		s_Area = th_box (pg, 24, 42, pw - 48, area_h ());
		const int ky = 372 + TH->dy, q = dx / 4;		// the keys: 52 px
		s_Freeze = th_key (pg, 8, ky, 150 + q, 52, "Freeze", TH_KEY_FUNC, key_cb, (void *) 0);
		th_key_font (s_Freeze, &lv_font_montserrat_20);
		s_TbDn = th_key (pg, 166 + q, ky, 80, 52, LV_SYMBOL_LEFT, TH_KEY, key_cb, (void *) 1);
		lv_obj_t *box = th_panel (pg, 254 + q, ky, 200 + q, 52, "", TH_PANEL_WINDOW);
		s_TbVal = th_label (box, 0, 13, &lv_font_montserrat_20, TH->win_ink, "");
		lv_obj_set_width (s_TbVal, 200 + q);
		lv_obj_set_style_text_align (s_TbVal, LV_TEXT_ALIGN_CENTER, 0);
		s_TbUp = th_key (pg, 462 + 2 * q, ky, 80, 52, LV_SYMBOL_RIGHT, TH_KEY, key_cb, (void *) 2);
		s_Gain = th_key (pg, 550 + 2 * q, ky, pw - 8 - 550 - 2 * q, 52, "Gain AUTO", TH_KEY, key_cb, (void *) 3);
		th_key_font (s_Gain, &lv_font_montserrat_20);
	}
	s_Meters = th_box (pg, strip_x (), sy, col_x (NCOL) - col_gap (), sh);
	lv_obj_add_flag (s_Meters, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Meters, meters_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Meters, meters_cb, LV_EVENT_ALL, 0);
	s_TrackKey = th_key (pg, strip_x () + col_x (NCOL), sy, strip_w () - col_x (NCOL), O ? sh : sh - 3, O ? "TRACK" : "Track",
			     TH_KEY_FUNC, key_cb, (void *) 4);
	if (!O) th_key_font (s_TrackKey, &lv_font_montserrat_16);
	lv_obj_add_event_cb (s_Area, area_draw, LV_EVENT_DRAW_MAIN, 0);
	keys_update ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_scope_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_scope_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

void sidecar_scope_debug (int source, int tb, int gain, int frozen)
{
	s_nSrc = source < -1 ? -1 : source >= SSC_SOURCES ? SSC_SOURCES - 1 : source;
	s_nTb = tb < 0 ? 0 : tb >= SSC_TBS ? SSC_TBS - 1 : tb;
	s_nGainSel = gain < 0 ? 0 : gain & 7;
	s_bFrozen = frozen != 0;
	if (!s_Page) return;
	keys_update ();
	lv_obj_invalidate (s_Area);
	lv_obj_invalidate (s_Meters);
}

static void meters_advance (const sidecar_scope_view_t *v)
{
	uint32_t now = lv_tick_get ();
	float dt = s_nLastMs ? (float) (uint32_t) (now - s_nLastMs) / 1000.0f : 0.0f;
	if (dt > 0.5f) dt = 0.5f;
	s_nLastMs = now;
	float fall = FALL_DB_S * dt;
	for (int ch = 0; ch < SSC_CH; ch++)
	{
		unsigned p = v->peak[ch];
		float db = p ? 20.0f * log10f ((float) p / (float) SSC_FS) : -120.0f;
		float d = s_Disp[ch] - fall;
		s_Disp[ch] = db > d ? db : d < -120.0f ? -120.0f : d;
		if (db >= s_Hold[ch]) { s_Hold[ch] = db; s_HoldAt[ch] = now; }
		else if (now - s_HoldAt[ch] > HOLD_MS)
		{
			float h = s_Hold[ch] - fall;
			s_Hold[ch] = h > s_Disp[ch] ? h : s_Disp[ch];
		}
		if (p >= SSC_FS - 8) s_ClipAt[ch] = now ? now : 1;
	}
}

static void gain_update (const sidecar_scope_view_t *v)	// AUTO without hunting (core/ui/page_scope.cpp's rule)
{
	if (s_nGainSel) { s_nShownG = 1 << (s_nGainSel - 1); return; }
	int peak = 0;
	for (int i = 0; i < v->ncols && i < SSC_COLS; i++)
	{
		int a = v->hi[i] > -v->lo[i] ? v->hi[i] : -v->lo[i];
		if (a > peak) peak = a;
	}
	int want = 1;
	while (want < 64 && peak * 2 * want < 31000) want *= 2;
	if (want < s_nAutoG) s_nAutoG = want, s_nAutoUp = 0;
	else if (want > s_nAutoG) { if (++s_nAutoUp >= AUTO_UP) s_nAutoG *= 2, s_nAutoUp = 0; }
	else s_nAutoUp = 0;
	s_nShownG = s_nAutoG;
}

void sidecar_scope_set (const sidecar_scope_view_t *v)
{
	if (!s_Page) return;
	int first = !s_bHave, track = !s_bHave || v->track != s_V.track || v->src != s_V.src || v->valid != s_V.valid ||
		    memcmp (v->name, s_V.name, sizeof v->name);
	meters_advance (v);
	if (!s_bFrozen || first || v->valid != s_V.valid)
	{
		int trace = first || v->gen != s_nTraceGen;
		if (trace)
		{
			// a sweep that only moved on (same source, timebase, gain): the columns written since the last view,
			// the cursor's gap and the peak's corner; anything else the whole trace
			int oc = s_V.cursor, part = !first && oc >= 0 && v->cursor >= 0 && v->src == s_V.src && v->tb == s_V.tb &&
						v->valid == s_V.valid && s_V.ncols == v->ncols;
			int g0 = s_nShownG;
			s_V = *v;
			s_nTraceGen = v->gen;
			gain_update (v);
			if (part && g0 == s_nShownG) area_inval_sweep (oc, v->cursor);
			else lv_obj_invalidate (s_Area);
		}
		else memcpy (s_V.peak, v->peak, sizeof v->peak);
	}
	else									// frozen: the trace held, the rest follows
	{
		s_V.track = v->track;
		memcpy (s_V.peak, v->peak, sizeof v->peak);
		memcpy (s_V.name, v->name, sizeof v->name);
	}
	s_bHave = 1;
	if (track)
	{
		keys_update ();
		lv_obj_invalidate (s_Meters);
		return;
	}
	for (int k = 0; k < NCOL; k++)				// a meter's strip only when what it shows moved
	{
		col_t cs;
		col_state (k, &cs);
		if (memcmp (&cs, &s_Drawn[k], sizeof cs))
		{
			int whole = cs.sel != s_Drawn[k].sel || cs.press != s_Drawn[k].press;
			s_Drawn[k] = cs;
			col_inval (k, whole);
		}
	}
}
