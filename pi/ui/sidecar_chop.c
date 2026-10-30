// pi/ui/sidecar_chop.c - see sidecar_chop.h. Code-drawn areas (the waveform, the pads, the list) and theme keys; every
// mark through th_fill / th_text / th_paint_key, so the OLED theme keeps its dots and CLASSIC its hardware look.
#include "sidecar_chop.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

static const char *const kTrack[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
static const char *const kTabs[] = {"A  1-16", "B  17-32", "C  33-48", "D  49-64", ""};
static const char *const kTabsO[] = {"A", "B", "C", "D", ""};
static const int kGrids[4] = {4, 8, 16, 32};
enum { K_LAZY, K_TAP, K_UNDO, K_GRID, K_TRANS, K_ADD, K_DEL, K_RSMP, K_N };
enum { E_UP, E_DN, E_LEFT, E_RIGHT, E_MINUS, E_PLUS, E_N };
#define ZOOMS 7

typedef struct { int x, y, w, h; } rc_t;
static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Wave, *s_Tabs, *s_Pads, *s_List, *s_EditLab, *s_Key[K_N], *s_Ed[E_N];
static rc_t r_wave, r_tabs, r_pads, r_list, r_edit, r_keys;
static sidecar_chop_view_t s_V;
static int s_bHave, s_nSel, s_nZoom, s_nBank, s_nFcol, s_nTop, s_nGridI = 2, s_nWant = -1;
static long s_nPan;
static int s_nPressPad = -1, s_nDrag = -1, s_bPanning;
static lv_point_t s_P0;
static long s_nPan0;
static unsigned s_nGen = ~0u;
static void (*s_Cmd) (const char *cmd);
static char s_Last[64];

static int oled (void) { return TH->id == THEME_OLED; }
static int U (void) { return oled () ? 1 : 2; }
static const lv_font_t *f_small (void) { return oled () ? TH->f_micro : &lv_font_montserrat_14; }
static const lv_font_t *f_body (void) { return oled () ? TH->f_small : &lv_font_montserrat_16; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }

static void cmd (const char *fmt, int a, int b, int c, int d)
{
	char t[64];
	lv_snprintf (t, sizeof t, fmt, a, b, c, d);
	lv_snprintf (s_Last, sizeof s_Last, "%s", t);
	if (s_Cmd) s_Cmd (t);
}
const char *sidecar_chop_last_command (void) { return s_Last; }
int sidecar_chop_sel (void) { return s_nSel; }
int sidecar_chop_wanted (void) { return s_nWant; }
int sidecar_chop_dragging (void) { return s_nDrag >= 0; }
void sidecar_chop_set_command (void (*fn) (const char *cmd)) { s_Cmd = fn; }

// ---- the window: the whole sample at zoom 0, then halves around the selection's middle (+ the pan)
void sidecar_chop_range (unsigned frames, unsigned *a, unsigned *b)
{
	if (!frames) { *a = 0, *b = 1; return; }
	unsigned len = frames >> s_nZoom;
	if (len < 256) len = frames < 256 ? frames : 256;
	long mid = frames / 2;
	if (s_bHave && s_V.n > 0 && s_nZoom)
	{
		int k = s_nSel < s_V.n ? s_nSel : s_V.n - 1;
		mid = (long) (s_V.start[k] + (s_V.end[k] - s_V.start[k]) / 2);
	}
	long x0 = mid - (long) len / 2 + s_nPan;
	if (x0 < 0) x0 = 0;
	if (x0 + (long) len > (long) frames) x0 = (long) frames - (long) len;
	*a = (unsigned) x0, *b = (unsigned) x0 + len;
}
static int x_of (unsigned f, int w)
{
	unsigned a = s_V.a, b = s_V.b > s_V.a ? s_V.b : s_V.a + 1;
	return (int) (((long long) f - a) * (w - 1) / (long long) (b - a));
}
static unsigned f_of (int x, int w)
{
	unsigned a = s_V.a, b = s_V.b > s_V.a ? s_V.b : s_V.a + 1;
	long long f = a + (long long) x * (b - a) / (w > 1 ? w - 1 : 1);
	return f < 0 ? 0 : (unsigned) f;
}
static void ms_text (unsigned frames, unsigned rate, char *b, int cap)
{
	unsigned ms10 = (unsigned) ((unsigned long long) frames * 10000 / (rate ? rate : 48000));
	if (ms10 < 10000) lv_snprintf (b, cap, "%u.%u", ms10 / 10, ms10 % 10);	// (1 s and up: whole ms)
	else lv_snprintf (b, cap, "%u", ms10 / 10);
}

// ---- the waveform
static void wave_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), w = lv_area_get_width (&c), h = lv_area_get_height (&c), u = U ();
	if (!O) th_fill (l, &c, 0, 0, w - 1, h - 1, TH->win, LV_OPA_COVER, 0);
	else th_frame (l, &c, 0, 0, w - 1, h - 1, DOT_DOTTED, 1, 0);
	const lv_font_t *fs = f_small ();
	int fh = lv_font_get_line_height (fs);
	if (!s_bHave || !s_V.valid || !s_V.ready || s_V.n <= 0)
	{
		const char *t = !s_bHave || !s_V.valid ? (O ? "NO CHOP VOICE: PICK CHOP ON A TRACK" : "No CHOP voice: pick CHOP on a track")
			      : !s_V.ready ? (O ? "LOADING THE SAMPLE" : "Loading the sample") : (O ? "NO SLICES" : "No slices");
		th_text (l, &c, 0, h / 2 - fh / 2, w, O ? TH->f_body : &lv_font_montserrat_18, c_dim (), LV_TEXT_ALIGN_CENTER, t);
		return;
	}
	int top = fh + 2 * u, mid = top + (h - top) / 2, amp = (h - top) / 2 - 2 * u;
	int k = s_nSel < s_V.n ? s_nSel : s_V.n - 1;
	if (!s_V.lazy)	// the selection, lit behind the outline
	{
		int x0 = x_of (s_V.start[k], w), x1 = x_of (s_V.end[k], w);
		if (x0 < 0) x0 = 0;
		if (x1 > w - 1) x1 = w - 1;
		if (x1 > x0) th_fill (l, &c, x0, top, x1, h - 2, O ? DOT_SHADE : th_mix (TH->win, TH->accent, 70), LV_OPA_COVER, 0);
	}
	th_fill (l, &c, 1, mid, w - 2, mid, O ? DOT_DOTTED : th_mix (TH->win, TH->win_dim, 90), LV_OPA_COVER, 0);
	for (int x = 1; x < w - 1 && s_V.ncols > 0; x++)	// the outline
	{
		int i = (x - 1) * s_V.ncols / (w - 2);
		if (i >= s_V.ncols) break;
		int a = mid - s_V.hi[i] * amp / 32768, b = mid - s_V.lo[i] * amp / 32768;
		if (a < top) a = top;
		if (b > h - 2) b = h - 2;
		if (b < a) b = a;
		th_fill (l, &c, x, a, x, b, c_ink (), LV_OPA_COVER, 0);
	}
	char t[24];
	for (int s = 0; s < s_V.n; s++)		// the markers: a slice's start, its number above
	{
		if (s_V.start[s] < s_V.a || s_V.start[s] >= s_V.b) continue;
		int x = x_of (s_V.start[s], w);
		uint32_t mc = s == s_nDrag ? (O ? TH->accent_hi : TH->accent) : s == k ? (O ? TH->ink : TH->accent) : c_dim ();
		th_fill (l, &c, x, top, x + (u > 1 && s == k ? 1 : 0), h - 2, mc, LV_OPA_COVER, 0);
		lv_snprintf (t, sizeof t, "%d", s + 1);
		int tw = th_text_width (fs, t);
		if (x + 2 + tw < w) th_text (l, &c, x + 2, u, tw + 2, fs, s == k ? c_ink () : c_dim (), LV_TEXT_ALIGN_LEFT, t);
	}
	if (s_V.lazy)
	{
		for (int i = 0; i < s_V.lazy_n; i++)
			if (s_V.lazy_at[i] >= s_V.a && s_V.lazy_at[i] < s_V.b)
			{
				int x = x_of (s_V.lazy_at[i], w);
				th_fill (l, &c, x, top, x, h - 2, O ? TH->accent : TH->warn, LV_OPA_COVER, 0);
			}
		if (s_V.lazy_pos >= s_V.a && s_V.lazy_pos < s_V.b)
		{
			int x = x_of (s_V.lazy_pos, w);
			th_fill (l, &c, x, top, x + u - 1, h - 2, TH->play, LV_OPA_COVER, 0);
		}
	}
	for (int i = 0; i < s_V.nheads; i++)	// the playheads
		if (s_V.head[i] >= s_V.a && s_V.head[i] < s_V.b)
		{
			int x = x_of (s_V.head[i], w);
			th_fill (l, &c, x, top, x + u - 1, h - 2, TH->play, LV_OPA_COVER, 0);
		}
	// the corner: the window, the zoom
	char z[32];
	unsigned ra = s_V.rate ? s_V.rate : 48000;
	lv_snprintf (z, sizeof z, O ? "Z%d %u-%u" : "zoom %d   %u-%u ms", s_nZoom, (unsigned) ((unsigned long long) s_V.a * 1000 / ra),
		  (unsigned) ((unsigned long long) s_V.b * 1000 / ra));
	th_text (l, &c, w / 2, h - fh - u, w / 2 - 2 * u, fs, c_dim (), LV_TEXT_ALIGN_RIGHT, z);	// (bottom right: the numbers on top)
}
static int marker_near (int x, int w)
{
	int best = -1, bd = 6 * U ();
	for (int s = 1; s < s_V.n; s++)
	{
		if (s_V.start[s] < s_V.a || s_V.start[s] >= s_V.b) continue;
		int d = x_of (s_V.start[s], w) - x;
		d = d < 0 ? -d : d;
		if (d <= bd) bd = d, best = s;
	}
	return best;
}
static void inval_all (void);
static void wave_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	if (!s_bHave || !s_V.valid || !s_V.ready || s_V.n <= 0) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (s_Wave, &c);
	int w = lv_area_get_width (&c), x = p.x - c.x1;
	if (code == LV_EVENT_PRESSED)
	{
		s_P0 = p, s_nPan0 = s_nPan, s_bPanning = 0;
		s_nDrag = s_V.lazy ? -1 : marker_near (x, w);
		lv_obj_invalidate (s_Wave);
	}
	else if (code == LV_EVENT_PRESSING)
	{
		int dx = p.x - s_P0.x;
		if (s_nDrag >= 0)
		{
			unsigned f = f_of (x, w);	// shown at once; the engine's map follows the command at the release
			if (f > s_V.start[s_nDrag - 1] + 64 && f + 64 < s_V.end[s_nDrag]) s_V.start[s_nDrag] = f, s_V.end[s_nDrag - 1] = f;
			lv_obj_invalidate (s_Wave);
		}
		else if (s_nZoom && (s_bPanning || dx > 6 * U () || dx < -6 * U ()))
		{
			s_bPanning = 1;
			s_nPan = s_nPan0 - (long) dx * (long) (s_V.b - s_V.a) / w;
		}
	}
	else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		if (s_nDrag >= 0 && code == LV_EVENT_RELEASED)
			cmd ("chop=start:%d:%d:%d", s_V.voice, s_nDrag + 1, (int) s_V.start[s_nDrag], 0);
		else if (!s_bPanning && code == LV_EVENT_RELEASED)	// a tap: the slice under it
		{
			unsigned f = f_of (x, w);
			for (int s = 0; s < s_V.n; s++)
				if (f >= s_V.start[s] && f < s_V.end[s]) s_nSel = s;
		}
		s_nDrag = -1, s_bPanning = 0;
		inval_all ();
	}
}

// ---- the pads
static void pad_rect (int i, int w, int h, int *x0, int *y0, int *x1, int *y1)
{
	int g = TH->gap, cw = (w - 3 * g) / 4, ch = (h - 3 * g) / 4;
	int col = i % 4, row = 3 - i / 4;	// pad 1 bottom left (the MPC's and the Rytm's way up)
	*x0 = col * (cw + g), *y0 = row * (ch + g), *x1 = *x0 + cw - 1, *y1 = *y0 + ch - 1;
}
static void pads_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	for (int i = 0; i < 16; i++)
	{
		int s = s_nBank * 16 + i, x0, y0, x1, y1;
		pad_rect (i, w, h, &x0, &y0, &x1, &y1);
		lv_area_t a = {c.x1 + x0, c.y1 + y0, c.x1 + x1, c.y1 + y1};
		char t[8], sub[16] = "";
		lv_snprintf (t, sizeof t, "%d", s + 1);
		int have = s_bHave && s_V.valid && s < s_V.n;
		if (have && s_V.lazy) lv_snprintf (sub, sizeof sub, oled () ? "TAP" : "tap");
		else if (have) ms_text (s_V.end[s] - s_V.start[s], s_V.rate, sub, sizeof sub);
		int led = !have ? 0 : s == s_V.played ? 52 : s == s_V.slc ? 47 : 43;
		th_paint_key (l, &a, t, sub, f_body (), TH_KEY_PAD, have && s == s_nSel, s == s_nPressPad, !have && !(s_bHave && s_V.lazy), led);
	}
}
static int pad_at (lv_point_t p)
{
	lv_area_t c;
	lv_obj_get_coords (s_Pads, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	for (int i = 0; i < 16; i++)
	{
		int x0, y0, x1, y1;
		pad_rect (i, w, h, &x0, &y0, &x1, &y1);
		if (p.x >= c.x1 + x0 && p.x <= c.x1 + x1 && p.y >= c.y1 + y0 && p.y <= c.y1 + y1) return i;
	}
	return -1;
}
static void pads_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	if (code == LV_EVENT_PRESSED)
	{
		int i = pad_at (p);
		if (i < 0 || !s_bHave || !s_V.valid) return;
		int s = s_nBank * 16 + i;
		s_nPressPad = s;
		if (s_V.lazy) cmd ("chop=lazy:%d:tap", s_V.voice, 0, 0, 0);		// at the press: the tap's moment
		else if (s < s_V.n) { s_nSel = s; cmd ("chop=play:%d:%d", s_V.voice, s + 1, 0, 0); }
		inval_all ();
	}
	else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		s_nPressPad = -1;
		lv_obj_invalidate (s_Pads);
	}
}
static void tabs_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) ud;
	s_nBank = idx & 3;
	lv_obj_invalidate (s_Pads);
}

// ---- the list (M8's): rows of slices, the cursor's column one of four settings of a page
static int row_h (void) { return lv_font_get_line_height (f_small ()) + (oled () ? 1 : 4); }
static int list_rows (void)
{
	lv_area_t c;
	lv_obj_get_coords (s_List, &c);
	return (lv_area_get_height (&c) - row_h () - 2) / row_h ();
}
static int nf (void) { return (oled () ? 1 : 2) + (TH->dx > 0); }	// the settings' columns: OLED 1, CLASSIC 2 (+1 wide)
static void col_x (int w, int *x)	// S START LEN F1..Fnf (x[3 + nf]: the end)
{
	int O = oled (), n = nf ();
	x[0] = 2 * U ();
	x[1] = x[0] + (O ? 15 : 30);
	int sw = O ? 33 : 62;			// "999.9" / "1367" (ms) in the list's font
	x[2] = x[1] + sw, x[3] = x[2] + sw;
	for (int i = 1; i <= n; i++) x[3 + i] = x[3] + (w - x[3]) * i / n;
}
static void list_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int O = oled (), w = lv_area_get_width (&c), rh = row_h (), u = U ();
	if (!O) th_fill (l, &c, 0, 0, w - 1, lv_area_get_height (&c) - 1, TH->win, LV_OPA_COVER, 0);
	else th_frame (l, &c, 0, 0, w - 1, lv_area_get_height (&c) - 1, DOT_DOTTED, 1, 0);
	const lv_font_t *fs = f_small ();
	int x[8];
	col_x (w, x);
	(void) x;
	int n = nf (), pg = s_nFcol / n;
	const char *hd[3] = {"S", O ? "START" : "start", O ? "LEN" : "len"};
	for (int i = 0; i < 3; i++) th_text (l, &c, x[i], 1, x[i + 1] - x[i], fs, c_dim (), LV_TEXT_ALIGN_LEFT, hd[i]);
	for (int j = 0; j < n; j++)
	{
		int f = pg * n + j;
		const char *nm = s_bHave && f < SCH_FIELDS ? s_V.fname[f] : "";
		th_text (l, &c, x[3 + j], 1, x[4 + j] - x[3 + j] - u, fs, f == s_nFcol ? c_ink () : c_dim (), LV_TEXT_ALIGN_LEFT, nm);
	}
	th_fill (l, &c, 1, rh, w - 2, rh, O ? DOT_DOTTED : c_dim (), LV_OPA_COVER, 0);
	if (!s_bHave || !s_V.valid) return;
	int nr = list_rows ();
	if (s_nSel < s_nTop) s_nTop = s_nSel;
	if (s_nSel >= s_nTop + nr) s_nTop = s_nSel - nr + 1;
	if (s_nTop < 0) s_nTop = 0;
	char t[24];
	for (int r = 0; r < nr && s_nTop + r < s_V.n; r++)
	{
		int s = s_nTop + r, y = rh + 2 + r * rh;
		int sel = s == s_nSel;
		if (sel) th_fill (l, &c, 1, y, w - 2, y + rh - 1, TH->sel_bg, LV_OPA_COVER, 0);
		uint32_t ink = sel ? TH->sel_ink : c_ink ();
		lv_snprintf (t, sizeof t, "%02d", s + 1);
		th_text (l, &c, x[0], y + 1, x[1] - x[0], fs, ink, LV_TEXT_ALIGN_LEFT, t);
		ms_text (s_V.start[s], s_V.rate, t, sizeof t);
		th_text (l, &c, x[1], y + 1, x[2] - x[1], fs, ink, LV_TEXT_ALIGN_LEFT, t);
		ms_text (s_V.end[s] - s_V.start[s], s_V.rate, t, sizeof t);
		th_text (l, &c, x[2], y + 1, x[3] - x[2], fs, ink, LV_TEXT_ALIGN_LEFT, t);
		for (int j = 0; j < n; j++)
		{
			int f = pg * n + j;
			if (f >= SCH_FIELDS) continue;
			if (sel && f == s_nFcol) th_frame (l, &c, x[3 + j] - u, y, x[4 + j] - 2 * u, y + rh - 1, ink, 1, 0);
			th_text (l, &c, x[3 + j], y + 1, x[4 + j] - x[3 + j] - 2 * u, fs, ink, LV_TEXT_ALIGN_LEFT, s_V.ftext[s][f]);
		}
	}
}
static void list_cb (lv_event_t *e)
{
	if (lv_event_get_code (e) != LV_EVENT_RELEASED || !s_bHave || !s_V.valid) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (s_List, &c);
	int r = (p.y - c.y1 - row_h () - 2) / row_h (), x[8], n = nf ();
	col_x (lv_area_get_width (&c), x);
	if (r >= 0 && s_nTop + r < s_V.n) s_nSel = s_nTop + r;
	for (int j = 0; j < n; j++)
		if (p.x - c.x1 >= x[3 + j] && p.x - c.x1 < x[4 + j] && (s_nFcol / n) * n + j < SCH_FIELDS) s_nFcol = (s_nFcol / n) * n + j;
	inval_all ();
}

// ---- the keys
static void keys_update (void)
{
	int O = oled ();
	char b[48], r[48];
	int have = s_bHave && s_V.valid && s_V.ready && s_V.n > 0;
	th_key_check (s_Key[K_LAZY], have && s_V.lazy);
	th_key_text (s_Key[K_LAZY], have && s_V.lazy ? (O ? "STOP" : "Stop") : (O ? "LAZY" : "Lazy"));
	th_key_enable (s_Key[K_TAP], have && s_V.lazy);
	lv_snprintf (b, sizeof b, O ? "GRID %d" : "Grid %d", kGrids[s_nGridI]);
	th_key_text (s_Key[K_GRID], b);
	int offer = have && s_V.rs_state == SCH_RS_OFFER;
	th_key_text (s_Key[K_RSMP], offer ? (O ? "YES" : "Replace") : (O ? "RSMP" : "Resample"));
	th_key_text (s_Key[K_DEL], offer ? (O ? "NO" : "Keep") : (O ? "DEL" : "Delete"));
	for (int i = 0; i < K_N; i++) if (i != K_TAP) th_key_enable (s_Key[i], have);
	for (int i = 0; i < E_N; i++) th_key_enable (s_Ed[i], have);
	// the title, the edit row's text
	if (s_bHave && s_V.valid)
	{
		lv_snprintf (b, sizeof b, O ? "CHOP  V%d %s" : "CHOP   V%d %s", s_V.voice + 1, s_V.track >= 0 && s_V.track < 12 ? kTrack[s_V.track] : "");
		char nm[24];
		th_fit_middle (nm, sizeof nm, O ? TH->f_body : &lv_font_montserrat_16, s_V.name, O ? 110 + TH->dx / 2 : 220 + TH->dx / 2);
		if (s_V.lazy) lv_snprintf (r, sizeof r, O ? "LAZY %d  TAP!" : "LAZY %d   tap the pads", s_V.lazy_n);
		else if (s_V.rs_state == SCH_RS_OFFER) lv_snprintf (r, sizeof r, O ? "S%d -> %s?" : "S%d -> %s: replace?", s_V.rs_slice + 1, s_V.rs_row);
		else lv_snprintf (r, sizeof r, O ? "%s  %d SL%s" : "%s   %d slices%s", nm, s_V.n, s_V.dirty ? " *" : "");
	}
	else lv_snprintf (b, sizeof b, "CHOP"), lv_snprintf (r, sizeof r, "%s", "");
	if (O) th_titlebar_set (s_TitleBar, b, r, 0);
	else { char t[96]; lv_snprintf (t, sizeof t, "%s   %s", b, r); th_label_set (s_Title, t); }
	if (s_bHave && s_V.valid && s_V.n > 0)
	{
		int k = s_nSel < s_V.n ? s_nSel : s_V.n - 1;
		lv_snprintf (b, sizeof b, O ? "S%d %s %s" : "Slice %d   %s  %s", k + 1, s_V.fname[s_nFcol], s_V.ftext[k][s_nFcol]);
	}
	else b[0] = 0;
	th_label_set (s_EditLab, b);
}
static void inval_all (void)
{
	lv_obj_invalidate (s_Wave);
	lv_obj_invalidate (s_Pads);
	lv_obj_invalidate (s_List);
	keys_update ();
}
static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	if (!s_bHave || !s_V.valid) return;
	int vi = s_V.voice, s = s_nSel < s_V.n ? s_nSel : s_V.n - 1;
	switch (k)
	{
	case K_LAZY: cmd (s_V.lazy ? "chop=lazy:%d:stop" : "chop=lazy:%d:arm", vi, 0, 0, 0); break;
	case K_TAP: cmd ("chop=lazy:%d:tap", vi, 0, 0, 0); break;
	case K_UNDO: cmd (s_V.lazy ? "chop=lazy:%d:undo" : "chop=undo:%d", vi, 0, 0, 0); break;
	case K_GRID: s_nGridI = (s_nGridI + 1) & 3; cmd ("chop=grid:%d:%d", vi, kGrids[s_nGridI], 0, 0); break;
	case K_TRANS: cmd ("chop=trans:%d:64", vi, 0, 0, 0); break;
	case K_ADD:
		if (s >= 0) cmd ("chop=split:%d:%d", vi, s + 1, 0, 0);
		break;
	case K_DEL:
		if (s_V.rs_state == SCH_RS_OFFER) cmd ("chop=replace:no", 0, 0, 0, 0);
		else if (s >= 0) cmd ("chop=del:%d:%d", vi, s + 1, 0, 0);
		break;
	default:
		if (s_V.rs_state == SCH_RS_OFFER) cmd ("chop=replace:yes", 0, 0, 0, 0);
		else if (s >= 0) cmd ("chop=resample:%d:%d", vi, s + 1, 0, 0);
		break;
	}
	inval_all ();
}
static void ed_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	if (!s_bHave || !s_V.valid || s_V.n <= 0) return;
	int s = s_nSel < s_V.n ? s_nSel : s_V.n - 1;
	switch (k)
	{
	case E_UP: if (s_nSel > 0) s_nSel--; break;
	case E_DN: if (s_nSel < s_V.n - 1) s_nSel++; break;
	case E_LEFT: if (s_nFcol > 0) s_nFcol--; break;
	case E_RIGHT: if (s_nFcol < SCH_FIELDS - 1) s_nFcol++; break;
	default:
	{
		int f = s_nFcol, v = s_V.field[s][f], st = s_V.fhi[f] - s_V.flo[f] > 200 ? 10 : 1;
		v += k == E_PLUS ? st : -st;
		v = v < s_V.flo[f] ? s_V.flo[f] : v > s_V.fhi[f] ? s_V.fhi[f] : v;
		s_V.field[s][f] = v;
		char t[64];
		lv_snprintf (t, sizeof t, "chop=set:%d:%d:%s:%d", s_V.voice, s + 1, s_V.fname[f], v);
		lv_snprintf (s_Last, sizeof s_Last, "%s", t);
		if (s_Cmd) s_Cmd (t);
		break;
	}
	}
	inval_all ();
}
static void zoom_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int d = (int) (intptr_t) ud;
	s_nZoom += d;
	s_nZoom = s_nZoom < 0 ? 0 : s_nZoom >= ZOOMS ? ZOOMS - 1 : s_nZoom;
	s_nPan = 0;
	inval_all ();
}

// ---- the page
static void layout (void)
{
	int O = oled (), u = U ();
	int pw = O ? TH->w - 48 : TH->w;
	int x0 = O ? 0 : 8, top = O ? 15 : 8, kh = O ? 22 : 52;
	int ph = O ? TH->h : 428 + TH->dy;
	int ky = ph - kh - (O ? 0 : 4);
	int lw = (pw - 2 * x0) * 58 / 100;
	r_wave = (rc_t) {x0, top, lw, (O ? 78 : 150) + TH->dy / 2};
	r_tabs = (rc_t) {x0, r_wave.y + r_wave.h + 2 * u, lw, O ? 12 : 30};
	r_pads = (rc_t) {x0, r_tabs.y + r_tabs.h + 2 * u, lw, ky - (r_tabs.y + r_tabs.h + 2 * u) - 3 * u};
	int lx = x0 + lw + 4 * u, lwid = pw - x0 - lx;
	int eh = O ? 34 : 76;
	r_list = (rc_t) {lx, top, lwid, ky - top - eh - 4 * u};
	r_edit = (rc_t) {lx, r_list.y + r_list.h + 2 * u, lwid, eh};
	r_keys = (rc_t) {x0, ky, pw - 2 * x0, kh};
}
void sidecar_chop_create (void *parent, int y0)
{
	int O = oled (), u = U ();
	int pw = O ? TH->w - 48 : TH->w;
	layout ();
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, O ? TH->h : 428 + TH->dy);
	s_Page = pg;
	s_nGen = ~0u, s_nGridI = 2;
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, 13);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		r_wave.y += 30, r_list.y += 30, r_list.h -= 30, r_wave.h -= 6;
		r_tabs.y += 24, r_pads.y += 24, r_pads.h -= 24;
		s_Title = th_label (pg, 12, 8, &lv_font_montserrat_18, TH->ink, "CHOP");
	}
	s_Wave = th_box (pg, r_wave.x, r_wave.y, r_wave.w, r_wave.h);
	lv_obj_add_flag (s_Wave, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Wave, wave_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Wave, wave_cb, LV_EVENT_ALL, 0);
	s_Tabs = th_seg (pg, r_tabs.x, r_tabs.y, r_tabs.w * 3 / 4, r_tabs.h, O ? kTabsO : kTabs, TH_KEY_SMALL, tabs_cb, 0);
	th_seg_set (s_Tabs, 0);
	th_key (pg, r_tabs.x + r_tabs.w * 3 / 4 + 2 * u, r_tabs.y, r_tabs.w / 8 - 2 * u, r_tabs.h, "-", TH_KEY | TH_KEY_SMALL, zoom_cb, (void *) (intptr_t) -1);
	th_key (pg, r_tabs.x + r_tabs.w * 7 / 8 + 2 * u, r_tabs.y, r_tabs.w / 8 - 2 * u, r_tabs.h, "+", TH_KEY | TH_KEY_SMALL, zoom_cb, (void *) (intptr_t) 1);
	s_Pads = th_box (pg, r_pads.x, r_pads.y, r_pads.w, r_pads.h);
	lv_obj_add_flag (s_Pads, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Pads, pads_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Pads, pads_cb, LV_EVENT_ALL, 0);
	s_List = th_box (pg, r_list.x, r_list.y, r_list.w, r_list.h);
	lv_obj_add_flag (s_List, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_List, list_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_List, list_cb, LV_EVENT_ALL, 0);
	s_EditLab = th_label (pg, r_edit.x + 2 * u, r_edit.y + u, O ? TH->f_small : &lv_font_montserrat_16, TH->ink, "");
	lv_obj_set_width (s_EditLab, r_edit.w - 4 * u);
	static const char *const kEd[E_N] = {LV_SYMBOL_UP, LV_SYMBOL_DOWN, LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT, "-", "+"};
	int ey = r_edit.y + (O ? 12 : 28), eh = r_edit.h - (O ? 12 : 28), ew = (r_edit.w - 5 * TH->gap) / E_N;
	for (int i = 0; i < E_N; i++)
		s_Ed[i] = th_key (pg, r_edit.x + i * (ew + TH->gap), ey, ew, eh, kEd[i], i >= E_MINUS ? TH_KEY_ACCENT : TH_KEY, ed_cb, (void *) (intptr_t) i);
	static const char *const kKeysO[K_N] = {"LAZY", "TAP", "UNDO", "GRID 16", "TRANS", "ADD", "DEL", "RSMP"};
	static const char *const kKeysC[K_N] = {"Lazy", "Tap", "Undo", "Grid 16", "Transient", "Add", "Delete", "Resample"};
	int kw = (r_keys.w - (K_N - 1) * TH->gap) / K_N;
	for (int i = 0; i < K_N; i++)
	{
		int kind = i == K_LAZY ? TH_KEY_FUNC | TH_KEY_HOLD : i == K_TAP ? TH_KEY_ACCENT : i == K_DEL ? TH_KEY_DANGER : TH_KEY;
		s_Key[i] = th_key (pg, r_keys.x + i * (kw + TH->gap), r_keys.y, kw, r_keys.h, O ? kKeysO[i] : kKeysC[i], kind, key_cb, (void *) (intptr_t) i);
		if (!O) th_key_font (s_Key[i], &lv_font_montserrat_16);
	}
	keys_update ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}
void sidecar_chop_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}
int sidecar_chop_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
void sidecar_chop_set (const sidecar_chop_view_t *v)
{
	if (!s_Page) return;
	if (s_bHave && v->gen == s_nGen && s_nDrag < 0) return;
	int drag = s_nDrag >= 0;
	unsigned ks = 0, ke = 0;
	if (drag) ks = s_V.start[s_nDrag], ke = s_V.end[s_nDrag - 1];	// (the marker being dragged stays where the finger is)
	s_V = *v;
	if (drag) s_V.start[s_nDrag] = ks, s_V.end[s_nDrag - 1] = ke;
	s_nGen = v->gen, s_bHave = 1;
	if (s_V.n > 0 && s_nSel >= s_V.n) s_nSel = s_V.n - 1;
	inval_all ();
}
int sidecar_chop_hit (int what, int i, int *x, int *y)
{
	lv_area_t c;
	if (!sidecar_chop_shown ()) return 0;
	switch (what)
	{
	case 0:
	{
		if (i < 1 || i > 16) return 0;
		lv_obj_get_coords (s_Pads, &c);
		int x0, y0, x1, y1;
		pad_rect (i - 1, lv_area_get_width (&c), lv_area_get_height (&c), &x0, &y0, &x1, &y1);
		*x = c.x1 + (x0 + x1) / 2, *y = c.y1 + (y0 + y1) / 2;
		return 1;
	}
	case 1:
	case 2:
		if (i < 0 || i >= (what == 1 ? K_N : E_N)) return 0;
		lv_obj_get_coords (what == 1 ? s_Key[i] : s_Ed[i], &c);
		*x = (c.x1 + c.x2) / 2, *y = (c.y1 + c.y2) / 2;
		return 1;
	case 3:
		lv_obj_get_coords (s_List, &c);
		*x = c.x1 + 6 * U (), *y = c.y1 + row_h () + 2 + i * row_h () + row_h () / 2;
		return i >= 0 && i < list_rows ();
	default:
		lv_obj_get_coords (s_Wave, &c);
		*x = c.x1 + lv_area_get_width (&c) * i / 100, *y = (c.y1 + c.y2) / 2;
		return 1;
	}
}
void sidecar_chop_debug (int sel, int zoom, int bank, int fcol)
{
	s_nSel = sel < 0 ? 0 : sel;
	s_nZoom = zoom < 0 ? 0 : zoom >= ZOOMS ? ZOOMS - 1 : zoom;
	s_nBank = bank & 3;
	s_nFcol = fcol < 0 ? 0 : fcol >= SCH_FIELDS ? SCH_FIELDS - 1 : fcol;
	s_nPan = 0;
	if (!s_Page) return;
	th_seg_set (s_Tabs, s_nBank);
	inval_all ();
}
