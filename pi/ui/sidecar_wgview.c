// pi/ui/sidecar_wgview.c - see sidecar_wgview.h. One code-drawn area (DRAW_MAIN), a title and an info line; every mark
// a filled rect through the theme (th_fill), so both themes draw it and the OLED theme keeps its dots. The POOL (a list's
// banks A..H, FILES aimed at a row, a row's menu, the P3 confirm, SELECT UNUSED): a second code-drawn area over the
// picture, its keys below; the POOL page is the same area with the three lists' tabs on top.
#include "sidecar_wgview.h"
#include "sidecar_theme.h"
#include "sidecar_vlabel.h"
#include "lvgl.h"
#include <string.h>

static lv_obj_t *s_Page, *s_Area, *s_Title, *s_TitleBar, *s_Info, *s_Info2;
static lv_obj_t *s_List, *s_Src, *s_Up, *s_Dn, *s_Done, *s_Mid;
static sidecar_wg_view_t s_V;
static unsigned s_nGen = ~0u;
static int s_nWant = -1;
static void (*s_Cmd) (const char *);
static void (*s_PageFn) (int);
static int s_bList, s_nTop = -1;		// the POOL shown (over the picture), its first row
static int s_bLib;				// FILES shown (over the rows)
// the rows: a bank's row 0..127; FILES: >= 0 an item of the folder, LROW_PATH the folder's path (a tap: the root),
// LROW_UP "..", LROW_MORE
#define LROW_PATH	(-1)
#define LROW_UP		(-2)
#define LROW_MORE	(-3)
static int s_Rows[WGV_LIB + 4], s_nRows;
static int list_kind (void) { return s_V.kind == WGV_GRANULAR || s_V.kind == WGV_WAVETABLE || s_V.kind == WGV_MULTISAMPLE; }

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_bg (void) { return oled () ? TH->bg : TH->win; }

// a line from (x0, y0) to (x1, y1) one column wide: the columns' vertical spans (no anti-aliasing)
static void seg (lv_layer_t *l, const lv_area_t *c, int x, int ya, int yb, uint32_t rgb, int w)
{
	int lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
	th_fill (l, c, x, lo, x + w - 1, hi, rgb, LV_OPA_COVER, 0);
}

static void draw_gran (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	const sidecar_wg_view_t *v = &s_V;
	int mid = h / 2, amp = h / 2 - 3, o = oled ();
	float pk = 0.05f;
	for (int i = 0; i < v->ncols; i++)
	{
		if (v->wmx[i] > pk) pk = v->wmx[i];
		if (-v->wmn[i] > pk) pk = -v->wmn[i];
	}
	uint32_t wave = c_dim ();
	int step = o ? 1 : 3;					// CLASSIC: a column every 3 px (the cost a third)
	int pa = mid, pb = mid;
	for (int x = 0; v->ncols > 0 && x < w; x += step)
	{
		int i = x * v->ncols / w;
		int a = mid - (int) (v->wmx[i] / pk * amp), b = mid - (int) (v->wmn[i] / pk * amp);
		if (o)						// the outline: the top and bottom envelopes as joined runs of dots
		{
			seg (l, c, x, x ? (pa < a ? pa + 1 : pa > a ? pa - 1 : a) : a, a, wave, 1);
			seg (l, c, x, x ? (pb < b ? pb + 1 : pb > b ? pb - 1 : b) : b, b, wave, 1);
			pa = a, pb = b;
		}
		else seg (l, c, x, a, b, th_mix (c_bg (), wave, 110), 1);
	}
	th_fill (l, c, 0, mid, w - 1, mid, th_mix (c_bg (), wave, 90), LV_OPA_COVER, 0);
	// the head, ANIM's reach as a bracket on top
	int xh = (int) (v->head * (float) (w - 1) + 0.5f);
	th_fill (l, c, xh, 0, xh + (o ? 0 : 1), h - 1, TH->accent, LV_OPA_COVER, 0);
	if (v->anim > 0)
	{
		int r = (int) (v->anim * (float) w * 0.25f);
		th_fill (l, c, xh - r, 0, xh + r, o ? 0 : 1, TH->accent, LV_OPA_COVER, 0);
	}
	// the grains
	int gw = o ? 3 : 4;
	for (int k = 0; k < v->n; k++)
	{
		int x = (int) (v->gpos[k] * (float) (w - gw));
		int gh = 1 + (int) (v->gamp[k] * (float) (amp - 1));
		uint32_t col = th_mix (wave, v->grev[k] ? TH->sub : c_ink (), 64 + (int) (v->gamp[k] * 192));
		th_fill (l, c, x, mid - gh, x + gw - 1, mid + gh, col, LV_OPA_COVER, 0);
	}
}

static void draw_wt (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	const sidecar_wg_view_t *v = &s_V;
	int o = oled ();
	int fw = w * 3 / 4, depth_x = w - fw - 2, depth_y = h * 2 / 5, a = h / 5, base = h - a - 6;
	float pk = 0.05f;
	for (int k = 0; k < WGV_STACK; k++)
		for (int i = 0; i < v->ncols; i++)
		{
			float y = v->stack[k][i] < 0 ? -v->stack[k][i] : v->stack[k][i];
			if (y > pk) pk = y;
		}
	int step = o ? 2 : 4;					// the frames behind: every other dot / 4 px
	for (int k = WGV_STACK - 1; k >= 0; k--)		// back (the last frames) to front (the first)
	{
		int dx = depth_x * k / (WGV_STACK - 1), dy = depth_y * k / (WGV_STACK - 1);
		uint32_t col = th_mix (c_bg (), c_dim (), 230 - k * 18);
		int py = base - dy - (int) (v->stack[k][0] / pk * a);
		for (int x = step; x < fw && v->ncols > 0; x += step)
		{
			int y = base - dy - (int) (v->stack[k][x * v->ncols / fw] / pk * a);
			seg (l, c, dx + x, py, y, col, step);
			py = y;
		}
	}
	// the frame at POS: at its depth, bright
	float d = v->pos * (float) (WGV_STACK - 1);
	int dx = (int) (depth_x * d / (WGV_STACK - 1)), dy = (int) (depth_y * d / (WGV_STACK - 1));
	int py = base - dy - (int) (v->cur[0] / pk * a);
	for (int x = 1; x < fw && v->ncols > 0; x++)
	{
		int y = base - dy - (int) (v->cur[x * v->ncols / fw] / pk * a);
		seg (l, c, dx + x, py, y, TH->accent, o ? 1 : 2);
		py = y;
	}
	// the place in the table
	int ty = h - 3;
	th_fill (l, c, 0, ty, w - 1, ty, c_dim (), LV_OPA_COVER, 0);
	int nt = v->frames > 64 ? 64 : v->frames;
	for (int f = 0; f < nt; f++)
	{
		int x = nt > 1 ? f * (w - 1) / (nt - 1) : 0;
		th_fill (l, c, x, ty - 1, x, ty - 1, c_dim (), LV_OPA_COVER, 0);
	}
	int xp = (int) (v->pos * (float) (w - 1));
	th_fill (l, c, xp - 1, ty - 2, xp + 1, h - 1, TH->accent, LV_OPA_COVER, 0);
}

// MULTISAMPLE: the zones as bars over a keyboard of the instrument's span (whole octaves, at least three), two rows
// alternating so neighbours read apart, each as tall as its velocity layers; a key sounding lit in the teal (CLASSIC:
// the accent) and its zone white; the roots as ticks. The same picture as VOICE's MULTISAMPLE panel, the machine
// view's size.
static int ms_black (int k) { int p = k % 12; return p == 1 || p == 3 || p == 6 || p == 8 || p == 10; }
static int ms_on (int k) { return k >= 0 && k < 128 && (s_V.ms_keys[k >> 3] >> (k & 7) & 1); }
static void draw_ms (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	const sidecar_wg_view_t *v = &s_V;
	int o = oled (), k0 = 127, k1 = 0;
	for (int z = 0; z < v->ms_nz; z++) { if (v->ms_lo[z] < k0) k0 = v->ms_lo[z]; if (v->ms_hi[z] > k1) k1 = v->ms_hi[z]; }
	for (int k = 0; k < 128; k++) if (ms_on (k)) { if (k < k0) k0 = k; if (k > k1) k1 = k; }
	if (k1 < k0) k0 = 24, k1 = 84;
	k0 = k0 / 12 * 12, k1 = k1 / 12 * 12 + 11;
	if (k1 - k0 < 35) k1 = k0 + 35 > 127 ? 127 : k0 + 35;
	const int kh = h * 2 / 5, zh = h - kh - (o ? 4 : 8), ky = h - kh;
	// the keys as a piano: seven white keys an octave over the width, the black keys narrower over their gaps (a key's
	// left and right edge: KX, KR)
	static const unsigned char kWi[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
	const int nw = (k1 / 12 - k0 / 12 + 1) * 7, base = (k0 / 12) * 7;
	const long ww256 = (long) w * 256 / nw;
	const int bw = (int) (ww256 * 3 / 5 / 256) | 1;
#define WI(k) ((k) / 12 * 7 + kWi[(k) % 12] - base)
#define WX(i) ((int) ((i) * ww256 / 256))
#define KX(k) (ms_black (k) ? WX (WI (k) + 1) - bw / 2 : WX (WI (k)))
#define KR(k) (ms_black (k) ? WX (WI (k) + 1) - bw / 2 + bw - 1 : WX (WI (k) + 1) - 1)
	for (int z = 0; z < v->ms_nz; z++)		// the zones: two rows alternating, as tall as their velocity layers
	{
		int xa = KX (v->ms_lo[z]), xb = KR (v->ms_hi[z]) - 1, row = z & 1, lay = v->ms_vel[z] > 4 ? 4 : v->ms_vel[z] < 1 ? 1 : v->ms_vel[z];
		int rh = zh / 2 - (o ? 2 : 4), bh = rh * lay / 4 < 3 ? 3 : rh * lay / 4, yb = row ? zh - 1 : zh / 2 - (o ? 2 : 4);
		int lit = 0;
		for (int k = v->ms_lo[z]; k <= v->ms_hi[z]; k++) lit |= ms_on (k);
		uint32_t col = lit ? (o ? TH->ink : 0xF0F0F0) : o ? th_oled_secondary (40 + 22 * lay) : th_mix (TH->win, TH->sub, 160 + 24 * lay);
		th_fill (l, c, xa, yb - bh + 1, xb < xa ? xa : xb, yb, col, LV_OPA_COVER, 0);
		int xr = (KX (v->ms_root[z]) + KR (v->ms_root[z])) / 2;
		th_fill (l, c, xr, yb - rh, xr, yb, lit ? TH->accent : c_dim (), LV_OPA_COVER, 0);
	}
	const lv_font_t *fl = o ? TH->f_micro : &lv_font_montserrat_12;
	for (int k = k0; k <= k1; k++)			// white keys first, their octave's C named
	{
		if (ms_black (k)) continue;
		th_fill (l, c, KX (k), ky, KR (k) - 1, h - 1, ms_on (k) ? TH->accent : o ? 0x848484 : 0xD8D8D8, LV_OPA_COVER, 0);
	}
	for (int k = (k0 + 11) / 12 * 12; k <= k1; k += 12)	// (after all of them: a name spans two keys)
	{
		char t[6];
		lv_snprintf (t, sizeof t, "C%d", k / 12 - 1);
		th_text (l, c, KX (k) + 1, h - lv_font_get_line_height (fl) - 1, 4 * (KR (k) - KX (k) + 1), fl, o ? 0x000000 : 0x25272A,
			 LV_TEXT_ALIGN_LEFT, t);
	}
	for (int k = k0; k <= k1; k++)			// then the black keys over them
		if (ms_black (k)) th_fill (l, c, KX (k), ky, KR (k), ky + kh * 3 / 5, ms_on (k) ? TH->accent : o ? 0x202020 : 0x2B2D30, LV_OPA_COVER, 0);
#undef WI
#undef WX
#undef KX
#undef KR
	if (!v->ms_ready)
		th_text (l, c, 0, zh / 2 - 6, w, o ? TH->f_body : &lv_font_montserrat_18, c_dim (), LV_TEXT_ALIGN_CENTER,
			 v->name[0] ? (o ? "LOADING" : "Loading") : (o ? "NO INSTRUMENT: TAP POOL" : "No instrument: tap POOL"));
}


// ---- the POOL (core/lib/pools.h): tab rows (the POOL page's lists, the banks), the bank's rows; FILES; a row's menu,
// the P3 confirm and the message row drawn over them
static int s_bPage;				// the POOL page (three tabs) instead of the machine view's list
static int s_nPageList;				// its tab (WGV_SAMPLES ..)
static int s_nBank;				// the bank shown (0 A .. 7 H)
static int s_nTarget = -1;			// FILES fills this code (-1 none)
static int s_bReplace;				// ... which holds an entry: REPLACE (back to the rows after one file)
static int s_bSelect;				// SELECT UNUSED
static unsigned char s_Desel[WGV_CODES / 8];	// unused rows a tap deselected (and those already sent)
static int s_nMenu = -1;			// the row whose menu is open (-1 none)
enum { ASK_NONE, ASK_CMD, ASK_UNUSED };
static int s_nAsk;				// the confirm open: YES sends s_AskCmd (ASK_CMD) or the selection (ASK_UNUSED)
static char s_Ask1[64], s_Ask2[64], s_AskCmd[192], s_AskDone[48];
static char s_Msg[80];				// the message row ("" none)
static int s_nUndo = -1;			// the list whose last replace / unload the message row's tap takes back
enum { M_REPLACE, M_UNLOAD, M_ACCEPT, M_CANCEL };
#define UNLOAD_PER	19			// rows a command ("pool=inst:unload:" + "A010:yes," each: the console's 191)
#define UNLOAD_CMDS	8			// commands a tap (the control core's queue holds 16)

static int row_h (void) { return oled () ? 18 : 31; }
static int bar_w (void) { return oled () ? 5 : 8; }
static int hdr (void) { return s_bLib ? 0 : s_bPage ? 2 : 1; }	// the tab rows on top (not scrolled)
static int nvis (void) { return lv_obj_get_height (s_List) / row_h (); }
static int msg_on (void) { return s_Msg[0] || (s_bSelect && !s_bLib); }
static int rows_shown (void) { int n = nvis () - hdr () - msg_on (); return n < 1 ? 1 : n; }

static int cur_list (void)
{
	if (s_bPage) return s_nPageList;
	return s_V.kind == WGV_GRANULAR ? WGV_SAMPLES : s_V.kind == WGV_WAVETABLE ? WGV_TABLES : s_V.kind == WGV_MULTISAMPLE ? WGV_INSTR : -1;
}
static const char *ltag (int l) { return l == WGV_TABLES ? "wt" : l == WGV_INSTR ? "inst" : "smp"; }	// pool=K:..
static const char *btag (int l) { return l == WGV_TABLES ? "wt" : l == WGV_INSTR ? "ms" : "gr"; }	// browse=K:..
static const char *mtag (void) { return s_V.kind == WGV_WAVETABLE ? "wt" : s_V.kind == WGV_MULTISAMPLE ? "msamp" : "gr"; }
static const char *lname (int l) { return l == WGV_TABLES ? "WAVETABLES" : l == WGV_INSTR ? "INSTRUMENTS" : "SAMPLES"; }
static const char *mname (void)
{
	return s_V.kind == WGV_GRANULAR ? "GRANULAR" : s_V.kind == WGV_WAVETABLE ? "WAVETABLE" : s_V.kind == WGV_MULTISAMPLE ? "MULTISAMPLE" : "";
}
static int rows_ok (void) { return s_V.list == cur_list () && s_V.bank == s_nBank; }	// the view holds the bank shown
static int r_type (int r) { return r == 0 ? WGV_RT_OFF : rows_ok () ? s_V.rtype[r] : WGV_RT_EMPTY; }
static int r_flags (int r) { return rows_ok () ? s_V.rflags[r] : 0; }
static int r_filled (int r) { return r > 0 && r_type (r) >= WGV_RT_SAMPLE; }
static int r_empty (int r) { return r > 0 && rows_ok () && s_V.rtype[r] == WGV_RT_EMPTY; }
static int r_users (int r) { return rows_ok () ? s_V.rusers[r] : 0; }
static int code_of (int r) { return s_nBank * 128 + (r & 127); }
static void row_text (int code, char *t) { lv_snprintf (t, 5, "%c%03d", 'A' + (code >> 7 & 7), code & 127); }
static void send (const char *c) { if (s_Cmd) s_Cmd (c); }
static void say (const char *t) { lv_snprintf (s_Msg, sizeof s_Msg, "%s", t); s_nUndo = -1; }
static int desel (int code) { return s_Desel[code >> 3 & 127] >> (code & 7) & 1; }
static int selected (int code)
{
	if (desel (code)) return 0;
	for (int i = 0; i < s_V.nunused && i < WGV_CODES; i++) if (s_V.unused[i] == code) return 1;
	return 0;
}
static int n_selected (void)
{
	int n = 0;
	for (int i = 0; i < s_V.nunused && i < WGV_CODES; i++) n += !desel (s_V.unused[i]);
	return s_V.list == cur_list () ? n : 0;
}
static int locked (void)			// the list cannot be edited: said
{
	if (!s_V.locked || s_V.list != cur_list ()) return 0;
	char t[80];
	lv_snprintf (t, sizeof t, "%s IS LOCKED: %s", lname (cur_list ()), s_V.lock_why[0] ? s_V.lock_why : "NOT READ");
	say (t);
	return 1;
}

static void rows_build (void)
{
	const sidecar_wg_view_t *v = &s_V;
	s_nRows = 0;
	if (s_bLib)
	{
		s_Rows[s_nRows++] = LROW_PATH;
		if (v->lib_cwd[0]) s_Rows[s_nRows++] = LROW_UP;
		for (int i = 0; i < v->lib_n && i < WGV_LIB; i++) s_Rows[s_nRows++] = i;
		if (v->lib_total > v->lib_n) s_Rows[s_nRows++] = LROW_MORE;
		return;
	}
	for (int r = 0; r < WGV_ROWS; r++) s_Rows[s_nRows++] = r;
}

static void top_clamp (void)
{
	int n = rows_shown ();
	if (s_nTop > s_nRows - n) s_nTop = s_nRows - n;
	if (s_nTop < 0) s_nTop = 0;
}

static void top_to_row (int r) { s_nTop = r - 2; top_clamp (); }
static void top_to_sel (void)			// the playing row two rows from the top (when it is in this bank)
{
	s_nTop = 0;
	if (!s_bLib && s_V.sel >= 0 && s_V.sel >> 7 == s_nBank) s_nTop = (s_V.sel & 127) - 2;
	top_clamp ();
}

static int next_empty (int after)		// the shown bank's first empty row after `after` (0: none)
{
	for (int r = after + 1; r < WGV_ROWS; r++) if (r_empty (r)) return r;
	return 0;
}

// the overlay box (a row's menu, the confirm): its rows, placed in the middle of the list area
static int menu_items (int *ids)
{
	int n = 0, r = s_nMenu, f = r_flags (r);
	if (r_type (r) == WGV_RT_SAMPLE || r_type (r) == WGV_RT_INSTR) ids[n++] = M_REPLACE;
	ids[n++] = M_UNLOAD;
	if (f & WGV_RF_CHANGED) ids[n++] = M_ACCEPT;
	ids[n++] = M_CANCEL;
	return n;
}
static int box_rows (void) { int ids[4]; return s_nAsk ? 3 : s_nMenu >= 0 ? menu_items (ids) + 1 : 0; }
static void box_geom (int w, int h, int *x0, int *y0, int *bw)
{
	int rh = row_h (), n = box_rows (), m = oled () ? 8 : 40;
	*x0 = m, *bw = w - bar_w () - 3 - 2 * m;
	*y0 = (h - n * rh) / 2;
}

static const char *menu_text (int id)
{
	int o = oled ();
	return id == M_REPLACE ? (o ? "REPLACE" : "Replace") : id == M_UNLOAD ? (o ? "UNLOAD" : "Unload")
	     : id == M_ACCEPT ? (o ? "ACCEPT THE CHANGED FILE" : "Accept the changed file") : (o ? "CANCEL" : "Cancel");
}

static void draw_box (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	int n = box_rows ();
	if (!n) return;
	int x0, y0, bw, rh = row_h (), od = oled ();
	const lv_font_t *fb = TH->f_body, *fs = od ? TH->f_micro : TH->f_small;
	box_geom (w, h, &x0, &y0, &bw);
	th_fill (l, c, x0 - 2, y0 - 2, x0 + bw + 1, y0 + n * rh + 1, c_bg (), LV_OPA_COVER, 0);
	th_frame (l, c, x0 - 2, y0 - 2, x0 + bw + 1, y0 + n * rh + 1, s_nAsk ? TH->warn : c_ink (), 1, 0);
	if (s_nAsk)
	{
		th_text (l, c, x0, y0 + (rh - fs->line_height) / 2, bw, fs, c_ink (), LV_TEXT_ALIGN_CENTER, s_Ask1);
		th_text (l, c, x0, y0 + rh + (rh - fs->line_height) / 2, bw, fs, c_ink (), LV_TEXT_ALIGN_CENTER, s_Ask2);
		int y = y0 + 2 * rh, hw = bw / 2;
		th_fill (l, c, x0 + 2, y + 1, x0 + hw - 2, y + rh - 2, TH->sel_bg, LV_OPA_COVER, 0);
		th_text (l, c, x0, y + (rh - fb->line_height) / 2, hw, fb, TH->sel_ink, LV_TEXT_ALIGN_CENTER, od ? "YES" : "Yes");
		th_frame (l, c, x0 + hw + 2, y + 1, x0 + bw - 2, y + rh - 2, c_dim (), 1, 0);
		th_text (l, c, x0 + hw, y + (rh - fb->line_height) / 2, hw, fb, c_ink (), LV_TEXT_ALIGN_CENTER, od ? "NO" : "No");
		return;
	}
	int ids[4], m = menu_items (ids);
	char t[48], row[5];
	row_text (code_of (s_nMenu), row);
	lv_snprintf (t, sizeof t, "%s  %s", row, rows_ok () ? s_V.rname[s_nMenu] : "");
	th_text (l, c, x0 + 4, y0 + (rh - fs->line_height) / 2, bw - 8, fs, c_dim (), LV_TEXT_ALIGN_LEFT, t);
	for (int i = 0; i < m; i++)
	{
		int y = y0 + (i + 1) * rh;
		th_fill (l, c, x0, y, x0 + bw - 1, y, od ? TH->faint : th_mix (c_bg (), c_dim (), 90), LV_OPA_COVER, 0);
		th_text (l, c, x0, y + (rh - fb->line_height) / 2, bw, fb, ids[i] == M_UNLOAD ? TH->warn : c_ink (), LV_TEXT_ALIGN_CENTER,
			 menu_text (ids[i]));
	}
}

static void state_text (int r, char *t, int cap)	// the right column: the loader's / the file's state, the voices on it
{
	int f = r_flags (r), u = r_users (r), ty = r_type (r);
	const char *s = f & WGV_RF_LOADING ? "LOADING" : f & WGV_RF_FAILED ? "FAIL" : f & WGV_RF_MISSING ? "MISSING"
		      : f & WGV_RF_CHANGED ? "CHANGED" : f & WGV_RF_RAM ? "RAM" : ty == WGV_RT_INPUT ? "LIVE" : ty == WGV_RT_BUILTIN ? "BUILT-IN" : "";
	const char *ch = f & WGV_RF_ST ? "ST" : f & WGV_RF_MONO ? "M" : "";	// docs/STEREO.md: the file's channels
	if (u) lv_snprintf (t, cap, "%s%s%s%sx%d", ch, ch[0] && s[0] ? "  " : "", s, s[0] || ch[0] ? "  " : "", u);
	else lv_snprintf (t, cap, "%s%s%s", ch, ch[0] && s[0] ? "  " : "", s);
}

static void list_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), rh = row_h (), od = oled ();
	const sidecar_wg_view_t *v = &s_V;
	const lv_font_t *fb = TH->f_body, *fs = od ? TH->f_micro : TH->f_small;
	int pad = od ? 3 : 10, bar = bar_w (), rw = w - bar - 3, nh = hdr (), body = rows_shown ();
	uint32_t line = od ? TH->faint : th_mix (c_bg (), c_dim (), 90);
	for (int i = 0; i < nh; i++)			// the tab rows: the POOL page's lists, then the banks A..H
	{
		int y = i * rh, list_row = s_bPage && i == 0, nt = list_row ? 3 : 8, cw = rw / nt;
		for (int b = 0; b < nt; b++)
		{
			char t[24];
			int on = list_row ? b == s_nPageList : b == s_nBank, x0 = b * cw;
			static const char *const kTab[3] = {"SAMPLES", "TABLES", "INSTR"};
			if (list_row) lv_snprintf (t, sizeof t, "%s %d", kTab[b], v->count[b]);
			else lv_snprintf (t, sizeof t, "%c", 'A' + b);
			if (on) th_fill (l, &c, x0 + 1, y + 1, x0 + cw - 2, y + rh - 2, TH->sel_bg, LV_OPA_COVER, 0);
			th_text (l, &c, x0, y + (rh - fb->line_height) / 2, cw, list_row ? fs : fb, on ? TH->sel_ink : c_dim (),
				 LV_TEXT_ALIGN_CENTER, t);
		}
		th_fill (l, &c, 0, y + rh - 1, rw, y + rh - 1, line, LV_OPA_COVER, 0);
	}
	for (int i = 0; i < body; i++)
	{
		int r = s_nTop + i;
		if (r >= s_nRows) break;
		int y = (nh + i) * rh, k = s_Rows[r];
		char t[WGV_LNAME + 112], rt[32];
		if (s_bLib)					// FILES: the path (and the row it fills), "..", folders, files
		{
			const char *right = "";
			uint32_t ink = c_ink (), rink = c_dim ();
			if (k == LROW_PATH)
			{
				char row[5] = "";
				if (s_nTarget >= 0) row_text (s_nTarget, row);
				lv_snprintf (t, sizeof t, "FILES  %s%s  %s %s", v->lib_cwd[0] ? v->lib_cwd : "/", v->lib_done ? "" : "  (READING)",
					     s_nTarget < 0 ? "" : s_bReplace ? "REPLACE" : "->", row);
				th_text (l, &c, pad, y + rh - fs->line_height - 1, rw - 2 * pad, fs, c_dim (), LV_TEXT_ALIGN_LEFT, t);
				th_fill (l, &c, 0, y + rh - 1, rw, y + rh - 1, line, LV_OPA_COVER, 0);
				continue;
			}
			if (k == LROW_UP) lv_snprintf (t, sizeof t, "%s  ..", od ? "<" : LV_SYMBOL_LEFT), right = "UP";
			else if (k == LROW_MORE) lv_snprintf (t, sizeof t, "%d MORE: THE WEB'S FILES TAB", v->lib_total - v->lib_n), ink = c_dim ();
			else if (v->lib_flags[k] & WGV_LIB_DIR) lv_snprintf (t, sizeof t, "%s/", v->lib_name[k]), right = od ? ">" : LV_SYMBOL_RIGHT;
			else
			{
				lv_snprintf (t, sizeof t, "%s", v->lib_name[k]);
				if (v->lib_row[k] >= 0)		// one entry per hash: where it is already
				{
					char row[5];
					row_text (v->lib_row[k], row);
					lv_snprintf (rt, sizeof rt, "IN %s", row);
					right = rt, rink = TH->accent;
				}
				else right = s_bReplace ? "REPLACE" : "SET";
			}
			th_text (l, &c, pad, y + (rh - fb->line_height) / 2, rw - 2 * pad - (od ? 48 : 90), fb, ink, LV_TEXT_ALIGN_LEFT, t);
			th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs, rink, LV_TEXT_ALIGN_RIGHT, right);
			th_fill (l, &c, 0, y + rh - 1, rw, y + rh - 1, od ? TH->bg : th_mix (c_bg (), c_dim (), 40), LV_OPA_COVER, 0);
			continue;
		}
		// a bank's row: the in-use marker (voices on it now: filled; seen playing: an outline; SELECT UNUSED: the
		// selection in the accent), "NNN  name" ("!name" missing, "~name" changed), the state right
		int code = code_of (k), on = code == v->sel && v->list == cur_list (), ty = r_type (k), f = r_flags (k);
		int empty = ty == WGV_RT_EMPTY, mw = od ? 4 : 8, my = y + (rh - mw) / 2;
		uint32_t ink = on ? TH->sel_ink : f & WGV_RF_MISSING ? TH->bad : f & WGV_RF_CHANGED ? TH->warn : empty ? c_dim () : c_ink ();
		if (on) th_fill (l, &c, 0, y + 1, rw, y + rh - 2, TH->sel_bg, LV_OPA_COVER, 0);
		if (s_bSelect && selected (code)) th_fill (l, &c, pad, my, pad + mw - 1, my + mw - 1, TH->accent, LV_OPA_COVER, 0);
		else if (r_users (k)) th_fill (l, &c, pad, my, pad + mw - 1, my + mw - 1, on ? ink : c_ink (), LV_OPA_COVER, 0);
		else if (f & WGV_RF_USED) th_frame (l, &c, pad, my, pad + mw - 1, my + mw - 1, on ? ink : c_dim (), 1, 0);
		const char *nm = k == 0 ? "OFF" : empty ? "---" : v->rname[k];
		lv_snprintf (t, sizeof t, "%03d  %s%s", k, f & WGV_RF_MISSING ? "!" : f & WGV_RF_CHANGED ? "~" : "", nm);
		th_text (l, &c, pad + mw + (od ? 3 : 6), y + (rh - fb->line_height) / 2, rw - 2 * pad - mw - (od ? 60 : 120), fb, ink,
			 LV_TEXT_ALIGN_LEFT, t);
		if (on && v->note[0]) lv_snprintf (rt, sizeof rt, "%s", v->note);
		else state_text (k, rt, sizeof rt);
		if (rt[0])
			th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs,
				 on ? ink : f & WGV_RF_LOADING ? c_dim () : f & WGV_RF_MISSING ? TH->bad : c_dim (), LV_TEXT_ALIGN_RIGHT, rt);
	}
	if (msg_on ())					// the message row (SELECT UNUSED: the warning), above the keys
	{
		int y = (nvis () - 1) * rh;
		char t[96];
		if (s_Msg[0]) lv_snprintf (t, sizeof t, "%s", s_Msg);
		else lv_snprintf (t, sizeof t, "%d SELECTED: %s", n_selected (), v->warn[0] ? v->warn : "NOT SEEN PLAYING");
		th_fill (l, &c, 0, y + 1, rw, y + rh - 1, od ? TH->faint : th_mix (c_bg (), TH->warn, 40), LV_OPA_COVER, 0);
		th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs, od ? TH->ink : TH->warn, LV_TEXT_ALIGN_LEFT, t);
	}
	// the scroll bar (the rows below the tab rows)
	int y0 = nh * rh, sh = body * rh;
	th_fill (l, &c, w - bar, y0, w - 1, y0 + sh - 1, od ? TH->faint : th_mix (c_bg (), c_dim (), 60), LV_OPA_COVER, 0);
	if (s_nRows > body)
	{
		int th = sh * body / s_nRows < 6 ? 6 : sh * body / s_nRows, ty = y0 + (sh - th) * s_nTop / (s_nRows - body);
		th_fill (l, &c, w - bar, ty, w - 1, ty + th - 1, c_ink (), LV_OPA_COVER, 0);
	}
	else th_fill (l, &c, w - bar, y0, w - 1, y0 + sh - 1, c_dim (), LV_OPA_COVER, 0);
	draw_box (l, &c, w, h);
}

static void list_keys (void);
static void title_update (void);
static void refresh (void) { rows_build (); top_clamp (); list_keys (); title_update (); lv_obj_invalidate (s_List); }

static void open_files (int code, int replace)	// FILES aimed at a row
{
	s_nTarget = code, s_bReplace = replace, s_bLib = 1, s_bList = 1, s_nTop = 0, s_nMenu = -1, s_nAsk = ASK_NONE;
	refresh ();
}

// P3: a replace / unload of row r - 0 go ahead (not in use), 1 asked (YES sends cmd + ":yes"), -1 refused (the Rytm plays)
static int guard (int r, const char *verb, const char *cmd, const char *done)
{
	int code = code_of (r), u = r_users (r), used = r_flags (r) & WGV_RF_USED;
	char row[5], t[80];
	row_text (code, row);
	if (!u && !used) return 0;
	if (s_V.playing)
	{
		lv_snprintf (t, sizeof t, "THE RYTM PLAYS: %s IS IN USE - STOP IT TO %s", row, verb);
		say (t);
		return -1;
	}
	if (u) lv_snprintf (s_Ask1, sizeof s_Ask1, "ROW %s USED BY %d SOUND%s.", row, u, u == 1 ? "" : "S");
	else lv_snprintf (s_Ask1, sizeof s_Ask1, "ROW %s SEEN PLAYING IN THIS PROJECT.", row);
	lv_snprintf (s_Ask2, sizeof s_Ask2, "%s?", verb);
	lv_snprintf (s_AskCmd, sizeof s_AskCmd, "%s:yes", cmd);
	lv_snprintf (s_AskDone, sizeof s_AskDone, "%s", done);
	s_nAsk = ASK_CMD;
	return 1;
}

static void edited (const char *cmd, const char *done)	// a replace / unload sent: the message row offers UNDO
{
	char t[80];
	send (cmd);
	lv_snprintf (t, sizeof t, "%s - TAP HERE TO UNDO", done);
	say (t);
	s_nUndo = cur_list ();
}

static void unload_selection (void)		// SELECT UNUSED's UNLOAD: a few commands of UNLOAD_PER rows
{
	char b[192];
	int l = cur_list (), sent = 0, cmds = 0, k = 0, inb = 0;
	for (int i = 0; i < s_V.nunused && i < WGV_CODES && cmds < UNLOAD_CMDS; i++)
	{
		int code = s_V.unused[i];
		if (desel (code)) continue;
		if (!inb) k = lv_snprintf (b, sizeof b, "pool=%s:unload:", ltag (l));
		char row[5];
		row_text (code, row);
		k += lv_snprintf (b + k, sizeof b - k, "%s%s:yes", inb ? "," : "", row);
		s_Desel[code >> 3 & 127] |= (unsigned char) (1u << (code & 7));	// (sent: not again before the view says)
		sent++;
		if (++inb == UNLOAD_PER) { send (b); cmds++; inb = 0; }
	}
	if (inb) send (b);
	int rest = n_selected ();
	char t[80];
	if (rest) lv_snprintf (t, sizeof t, "%d UNLOADED, %d LEFT: UNLOAD AGAIN", sent, rest);
	else lv_snprintf (t, sizeof t, "%d UNLOADED", sent);
	if (!rest) s_bSelect = 0;
	say (t);
}

static void menu_do (int id)
{
	int r = s_nMenu, l = cur_list ();
	char cmd[64], row[5], done[32];
	s_nMenu = -1;
	if (id == M_CANCEL || r < 0) return;
	if (locked ()) return;
	row_text (code_of (r), row);
	if (id == M_ACCEPT)
	{
		lv_snprintf (cmd, sizeof cmd, "pool=%s:accept:%s", ltag (l), row);
		send (cmd);
		lv_snprintf (done, sizeof done, "%s: THE NEW FILE ACCEPTED", row);
		say (done);
		return;
	}
	if (id == M_REPLACE)				// P3 is asked when the file is chosen; refused now if the Rytm plays
	{
		if (s_V.playing && (r_users (r) || (r_flags (r) & WGV_RF_USED)))
		{
			char t[80];
			lv_snprintf (t, sizeof t, "THE RYTM PLAYS: %s IS IN USE - STOP IT TO REPLACE", row);
			say (t);
			return;
		}
		open_files (code_of (r), 1);
		return;
	}
	lv_snprintf (cmd, sizeof cmd, "pool=%s:unload:%s", ltag (l), row);
	lv_snprintf (done, sizeof done, "%s UNLOADED", row);
	if (guard (r, "UNLOAD", cmd, done) == 0) edited (cmd, done);
}

static void ask_yes (void)
{
	int k = s_nAsk;
	s_nAsk = ASK_NONE;
	if (k == ASK_UNUSED) { unload_selection (); return; }
	if (k == ASK_CMD)
	{
		edited (s_AskCmd, s_AskDone);
		if (s_bLib && s_bReplace) s_bLib = 0, s_nTarget = -1, s_bReplace = 0;
	}
}

static void file_tap (int k)			// FILES: a file / virtual item into the target row
{
	const sidecar_wg_view_t *v = &s_V;
	int l = cur_list ();
	char cmd[64], row[5], t[80];
	if (s_nTarget < 0) return;
	if (v->lib_row[k] >= 0 && v->lib_row[k] != s_nTarget)	// one entry per hash: go to where it is
	{
		row_text (v->lib_row[k], row);
		lv_snprintf (t, sizeof t, "ALREADY IN THE POOL AT %s", row);
		s_bLib = 0, s_nTarget = -1, s_bReplace = 0;
		s_nBank = v->lib_row[k] >> 7 & 7;
		say (t);
		rows_build ();
		top_to_row (v->lib_row[k] & 127);
		return;
	}
	if (locked ()) return;
	row_text (s_nTarget, row);
	lv_snprintf (cmd, sizeof cmd, "pool=%s:assign:%s:%d", ltag (l), row, k);
	if (s_bReplace)
	{
		char done[32];
		lv_snprintf (done, sizeof done, "%s REPLACED", row);
		int g = guard (s_nTarget & 127, "REPLACE", cmd, done);
		if (g == 0) { edited (cmd, done); s_bLib = 0, s_nTarget = -1, s_bReplace = 0; }
		else if (g < 0) s_bLib = 0, s_nTarget = -1, s_bReplace = 0;
		return;
	}
	send (cmd);
	lv_snprintf (t, sizeof t, "%s: %s", row, v->lib_name[k]);
	say (t);
	int nx = s_nTarget >> 7 == s_nBank ? next_empty (s_nTarget & 127) : 0;	// the next empty row then
	if (nx) s_nTarget = code_of (nx);
	else s_bLib = 0, s_nTarget = -1;
}

static int long_done;				// a long press took this touch (no tap after it)
static void list_tap (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	if (long_done) { long_done = 0; return; }
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int x = p.x - c.x1, y = p.y - c.y1, rh = row_h (), i = y / rh, w = lv_area_get_width (&c), nh = hdr ();
	char cmd[96];
	if (s_nAsk || s_nMenu >= 0)			// the box: an item, or outside it: closed
	{
		int x0, y0, bw, n = box_rows ();
		box_geom (w, lv_area_get_height (&c), &x0, &y0, &bw);
		int bi = y >= y0 && x >= x0 && x < x0 + bw ? (y - y0) / rh : -1;
		if (s_nAsk) { if (bi == 2 && x < x0 + bw / 2) ask_yes (); else s_nAsk = ASK_NONE; }
		else
		{
			int ids[4];
			menu_items (ids);
			if (bi >= 1 && bi < n) menu_do (ids[bi - 1]); else s_nMenu = -1;
		}
		refresh ();
		return;
	}
	if (msg_on () && i == nvis () - 1)		// the message row: UNDO when it offers it
	{
		if (s_nUndo >= 0)
		{
			lv_snprintf (cmd, sizeof cmd, "pool=%s:undo", ltag (s_nUndo));
			send (cmd);
			say ("UNDONE");
		}
		else s_Msg[0] = 0;
		refresh ();
		return;
	}
	s_Msg[0] = 0, s_nUndo = -1;
	if (i < nh)					// the tab rows
	{
		int rw = w - bar_w () - 3, list_row = s_bPage && i == 0, nt = list_row ? 3 : 8, cw = rw / nt, b = x / (cw > 0 ? cw : 1);
		b = b < 0 ? 0 : b >= nt ? nt - 1 : b;
		if (list_row) sidecar_wgview_set_page_list (b);
		else
		{
			sidecar_wgview_set_bank (b);
			lv_snprintf (cmd, sizeof cmd, "pool=%s:shown:%c", ltag (cur_list ()), 'A' + b);
			send (cmd);
		}
		refresh ();
		return;
	}
	int r = s_nTop + i - nh;
	if (i >= nh + rows_shown () || r < 0 || r >= s_nRows) { refresh (); return; }
	int k = s_Rows[r], l = cur_list ();
	if (l < 0) return;
	if (s_bLib)
	{
		if (k == LROW_PATH) lv_snprintf (cmd, sizeof cmd, "browse=%s:root", btag (l)), send (cmd);
		else if (k == LROW_UP) lv_snprintf (cmd, sizeof cmd, "browse=%s:up", btag (l)), send (cmd);
		else if (k >= 0 && (s_V.lib_flags[k] & WGV_LIB_DIR)) lv_snprintf (cmd, sizeof cmd, "browse=%s:open:%d", btag (l), k), send (cmd);
		else if (k >= 0) file_tap (k);
		refresh ();
		return;
	}
	if (s_bSelect)					// SELECT UNUSED: a selected row deselected (and back)
	{
		int code = code_of (k);
		if (r_filled (k)) s_Desel[code >> 3 & 127] ^= (unsigned char) (1u << (code & 7));
		refresh ();
		return;
	}
	if (r_empty (k)) { if (!locked ()) open_files (code_of (k), 0); refresh (); return; }	// an empty row: FILES aimed at it
	if (!rows_ok ()) return;
	if (s_bPage) { if (k > 0) s_nMenu = k; refresh (); return; }	// the POOL page: the row's menu
	char row[5];
	row_text (code_of (k), row);					// the voice plays it (row 0: OFF)
	lv_snprintf (cmd, sizeof cmd, "%s=pick:%d:%s", mtag (), s_V.voice, row);
	send (cmd);
	refresh ();
}

static void list_long (lv_event_t *e)		// a long press on a filled row: its menu (REPLACE / UNLOAD)
{
	lv_obj_t *o = lv_event_get_target (e);
	if (s_bLib || s_bSelect || s_nAsk || s_nMenu >= 0) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int i = (p.y - c.y1) / row_h (), r = s_nTop + i - hdr ();
	if (i < hdr () || i >= hdr () + rows_shown () || r < 0 || r >= s_nRows) return;
	int k = s_Rows[r];
	if (r_filled (k)) s_nMenu = k, s_Msg[0] = 0;
	else if (r_empty (k) && !locked ()) open_files (code_of (k), 0);
	long_done = 1;
	refresh ();
}

static int fill_ok (void)			// FILL B: a card folder into the shown bank's empty rows (not instruments)
{
	return s_bLib && !s_bReplace && cur_list () != WGV_INSTR && s_V.lib_cwd[0] == '/';
}

static void list_keys (void)
{
	int on = s_bList && (list_kind () || s_bPage), o = oled ();
	if (on) { lv_obj_remove_flag (s_List, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag (s_Area, LV_OBJ_FLAG_HIDDEN); }
	else { lv_obj_add_flag (s_List, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag (s_Area, LV_OBJ_FLAG_HIDDEN); }
	lv_obj_t *ks[3] = {s_Up, s_Dn, s_Done};
	for (int i = 0; i < 3; i++)
		if (on) lv_obj_remove_flag (ks[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (ks[i], LV_OBJ_FLAG_HIDDEN);
	char t[24];
	int mid = 1;
	if (!on) lv_snprintf (t, sizeof t, "%s", o ? "ALL POOLS" : "ALL POOLS");
	else if (s_bLib) { mid = fill_ok (); lv_snprintf (t, sizeof t, "FILL %c", 'A' + s_nBank); }
	else if (s_bSelect) lv_snprintf (t, sizeof t, "UNLOAD %d", n_selected ());
	else lv_snprintf (t, sizeof t, "%s", o ? "UNUSED" : "SELECT UNUSED");
	th_key_text (s_Mid, t);
	if (mid) lv_obj_remove_flag (s_Mid, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Mid, LV_OBJ_FLAG_HIDDEN);
	th_key_text (s_Done, s_bLib ? "BACK" : s_bSelect ? "CANCEL" : "DONE");
	if (on || !list_kind ()) lv_obj_add_flag (s_Src, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag (s_Src, LV_OBJ_FLAG_HIDDEN);
	if (on) { lv_obj_add_flag (s_Info, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag (s_Info2, LV_OBJ_FLAG_HIDDEN); }
	else { lv_obj_remove_flag (s_Info, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag (s_Info2, LV_OBJ_FLAG_HIDDEN); }
}

static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	char cmd[160];
	if (k == 0) { sidecar_wgview_list (1); return; }
	s_nMenu = -1, s_nAsk = ASK_NONE;
	if (k == 3)					// DONE / BACK / CANCEL
	{
		s_Msg[0] = 0, s_nUndo = -1;
		if (s_bLib) { s_bLib = 0, s_nTarget = -1, s_bReplace = 0; rows_build (); top_to_sel (); refresh (); return; }
		if (s_bSelect) { s_bSelect = 0; refresh (); return; }
		if (s_bPage) { if (s_PageFn) s_PageFn (0); return; }
		sidecar_wgview_list (0);
		return;
	}
	if (k == 4)					// the middle key
	{
		if (!s_bList || (!list_kind () && !s_bPage)) { if (s_PageFn) s_PageFn (1); return; }	// ALL POOLS
		if (s_bLib)				// FILL B
		{
			if (!fill_ok () || locked ()) { refresh (); return; }
			lv_snprintf (cmd, sizeof cmd, "pool=%s:fill:%c:%s", ltag (cur_list ()), 'A' + s_nBank, s_V.lib_cwd);
			send (cmd);
			lv_snprintf (cmd, sizeof cmd, "BANK %c: FILLING FROM %s", 'A' + s_nBank, s_V.lib_cwd);
			say (cmd);
			s_bLib = 0, s_nTarget = -1;
			refresh ();
			return;
		}
		if (s_bSelect)				// UNLOAD N: asked first, with the warning
		{
			int n = n_selected ();
			if (!n) { say ("NOTHING SELECTED"); refresh (); return; }
			if (locked ()) { refresh (); return; }
			lv_snprintf (s_Ask1, sizeof s_Ask1, "UNLOAD %d ROW%s FROM %s?", n, n == 1 ? "" : "S", lname (cur_list ()));
			lv_snprintf (s_Ask2, sizeof s_Ask2, "%s", s_V.warn[0] ? s_V.warn : "NOT SEEN PLAYING");
			s_nAsk = ASK_UNUSED;
			refresh ();
			return;
		}
		if (locked ()) { refresh (); return; }
		s_bSelect = 1, s_Msg[0] = 0, s_nUndo = -1;	// SELECT UNUSED
		memset (s_Desel, 0, sizeof s_Desel);
		refresh ();
		return;
	}
	s_nTop += (k == 1 ? -1 : 1) * (rows_shown () - 1);
	top_clamp ();
	lv_obj_invalidate (s_List);
}

void sidecar_wgview_set_command (void (*fn) (const char *cmd)) { s_Cmd = fn; }
void sidecar_wgview_set_page (void (*fn) (int open)) { s_PageFn = fn; }
int sidecar_wgview_list_shown (void) { return s_Page && s_bList && (list_kind () || s_bPage); }
int sidecar_wgview_lib_shown (void) { return sidecar_wgview_list_shown () && s_bLib; }
int sidecar_wgview_bank (void) { return s_nBank; }
int sidecar_wgview_pool_page_shown (void) { return s_Page && s_bPage; }
int sidecar_wgview_page_list (void) { return s_nPageList; }
int sidecar_wgview_list_id (void) { return cur_list (); }
int sidecar_wgview_selecting (void) { return s_bList && s_bSelect; }
int sidecar_wgview_target (void) { return s_bLib ? s_nTarget : -1; }
const char *sidecar_wgview_message (void) { return s_nAsk ? s_Ask1 : s_Msg; }
int sidecar_wgview_asking (void) { return s_nAsk ? 1 : s_nMenu >= 0 ? 2 : 0; }
void sidecar_wgview_set_bank (int bank)
{
	s_nBank = bank < 0 ? 0 : bank > 7 ? 7 : bank;
	if (!s_Page) return;
	s_nMenu = -1;
	rows_build ();
	top_to_sel ();
	lv_obj_invalidate (s_List);
}
void sidecar_wgview_set_page_list (int list)
{
	s_nPageList = list < 0 ? 0 : list >= WGV_LISTS ? WGV_LISTS - 1 : list;
	s_bLib = 0, s_bSelect = 0, s_nTarget = -1, s_nMenu = -1, s_nAsk = ASK_NONE;
	if (!s_Page) return;
	s_nTop = 0;
	refresh ();
}
static void modes_off (void) { s_bLib = 0, s_bSelect = 0, s_nTarget = -1, s_bReplace = 0, s_nMenu = -1, s_nAsk = ASK_NONE, s_Msg[0] = 0, s_nUndo = -1; }
void sidecar_wgview_list (int on)
{
	if (!s_Page) return;
	s_bList = on;
	modes_off ();
	if (on && s_V.sel >= 0 && s_V.sel < WGV_CODES && s_V.list == cur_list ()) s_nBank = s_V.sel >> 7;	// the bank the voice plays from
	rows_build ();
	top_to_sel ();
	list_keys ();
	title_update ();
	lv_obj_invalidate (s_List);
}
void sidecar_wgview_lib (int on)
{
	if (!s_Page) return;
	if (on) { int r = next_empty (0); open_files (r ? code_of (r) : -1, 0); return; }
	s_bLib = 0, s_nTarget = -1, s_bReplace = 0;
	rows_build ();
	top_to_sel ();
	refresh ();
}
void sidecar_wgview_pool_page (int on)
{
	if (!s_Page) return;
	s_bPage = on;
	s_bList = on;
	modes_off ();
	s_nTop = 0;
	rows_build ();
	top_clamp ();
	list_keys ();
	title_update ();
	lv_obj_invalidate (s_List);
}

static void area_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c);
	if (s_V.kind == WGV_GRANULAR) draw_gran (l, &c, w, h);
	else if (s_V.kind == WGV_WAVETABLE) draw_wt (l, &c, w, h);
	else if (s_V.kind == WGV_MULTISAMPLE) draw_ms (l, &c, w, h);
}

void sidecar_wgview_create (void *parent, int y0)
{
	int o = oled ();
	s_nGen = ~0u;
	memset (&s_V, 0, sizeof s_V);
	s_V.list = -1, s_V.sel = -1;
	// 1024x600: the picture and the list take what the screen adds (TH->dx, dy: more rows), the keys get wider
	const int dx = TH->dx, dy = TH->dy, q = dx / 4;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, o ? TH->w - 48 : TH->w, o ? TH->h : 428 + dy);
	s_Page = pg;
	if (o)
	{
		const int ky = 204 + dy;
		s_TitleBar = th_titlebar (pg, 0, 0, 352 + dx, 13);
		th_titlebar_set (s_TitleBar, "MACHINE", "", 0);
		s_Title = 0;
		s_Area = th_box (pg, 3, 18, 346 + dx, 178 + dy);
		s_List = th_box (pg, 0, 16, 352 + dx, 180 + dy);
		s_Info = th_label (pg, 3, ky, TH->f_micro, TH->dim, "");
		s_Info2 = th_label (pg, 3, ky + 18, TH->f_micro, TH->dim, "");
		s_Src = th_key (pg, 264 + 3 * q, ky, 88 + q, 34, "POOL", TH_KEY_FUNC | TH_KEY_SMALL, key_cb, (void *) 0);
		s_Up = th_key (pg, 0, ky, 84 + q, 34, "UP", TH_KEY | TH_KEY_SMALL, key_cb, (void *) 1);
		s_Dn = th_key (pg, 90 + q, ky, 84 + q, 34, "DOWN", TH_KEY | TH_KEY_SMALL, key_cb, (void *) 2);
		s_Done = th_key (pg, 264 + 3 * q, ky, 88 + q, 34, "DONE", TH_KEY_ACCENT | TH_KEY_SMALL, key_cb, (void *) 3);
		s_Mid = th_key (pg, 180 + 2 * q, ky, 78 + q, 34, "ALL POOLS", TH_KEY_FUNC | TH_KEY_SMALL, key_cb, (void *) 4);
	}
	else
	{
		const int ky = 372 + dy;
		s_TitleBar = 0;
		th_panel (pg, 8, 6, 784 + dx, 360 + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "MACHINE");
		s_Area = th_box (pg, 24, 44, 752 + dx, 310 + dy);
		s_List = th_box (pg, 24, 44, 752 + dx, 310 + dy);
		s_Info = th_label (pg, 16, ky + 4, &lv_font_montserrat_18, TH->ink, "");
		s_Info2 = th_label (pg, 16, ky + 30, &lv_font_montserrat_14, TH->dim, "");
		s_Src = th_key (pg, 616 + 3 * q, ky, 176 + q, 52, "POOL", TH_KEY_FUNC, key_cb, (void *) 0);
		s_Up = th_key (pg, 16, ky, 140 + q, 52, "UP", TH_KEY, key_cb, (void *) 1);
		s_Dn = th_key (pg, 164 + q, ky, 140 + q, 52, "DOWN", TH_KEY, key_cb, (void *) 2);
		s_Done = th_key (pg, 616 + 3 * q, ky, 176 + q, 52, "DONE", TH_KEY_ACCENT, key_cb, (void *) 3);
		s_Mid = th_key (pg, 312 + 2 * q, ky, 296 + q, 52, "ALL POOLS", TH_KEY_FUNC, key_cb, (void *) 4);
	}
	lv_obj_set_width (s_Info, (o ? 174 : 290) + 2 * q);
	lv_obj_set_width (s_Info2, (o ? 174 : 290) + 2 * q);
	lv_obj_add_event_cb (s_Area, area_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_List, list_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_flag (s_List, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_List, list_tap, LV_EVENT_CLICKED, 0);
	lv_obj_add_event_cb (s_List, list_long, LV_EVENT_LONG_PRESSED, 0);
	s_bList = 0, s_bLib = 0, s_bPage = 0, s_nTop = -1, s_nBank = 0, long_done = 0;
	modes_off ();
	list_keys ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_wgview_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_wgview_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
void sidecar_wgview_want (int voice) { s_nWant = voice; }
int sidecar_wgview_wanted (void) { return s_nWant; }

// the title: the picture's machine, voice and source; the POOL's list and the RAM figure ("MB and %", "-" unknown)
static void title_update (void)
{
	const sidecar_wg_view_t *v = &s_V;
	char t[64], r[48];
	if (s_bList && (list_kind () || s_bPage))
	{
		int l = cur_list ();
		if (s_bPage) lv_snprintf (t, sizeof t, "POOL  %s", lname (l));
		else lv_snprintf (t, sizeof t, "%s  %s %s", lname (l), mname (), kVoiceName[v->voice & 7]);
		if (s_bSelect) lv_snprintf (r, sizeof r, "%d SELECTED", n_selected ());
		else if (v->locked && v->list == l) lv_snprintf (r, sizeof r, "LOCKED");
		else if (v->ram_known) lv_snprintf (r, sizeof r, "RAM %u MB %d%%", v->ram_mb, v->ram_pct);
		else lv_snprintf (r, sizeof r, "RAM -");
	}
	else
	{
		const char *k = v->kind ? mname () : "NO WAVETABLE / GRANULAR / MULTISAMPLE VOICE";
		if (v->kind) lv_snprintf (t, sizeof t, "%s  %s", k, kVoiceName[v->voice & 7]);
		else lv_snprintf (t, sizeof t, "%s", k);
		lv_snprintf (r, sizeof r, "%s", v->kind ? v->name : "");
	}
	if (s_TitleBar) th_titlebar_set (s_TitleBar, t, r, 0);
	else
	{
		char i2[120];
		lv_snprintf (i2, sizeof i2, "%s   %s", t, r);
		th_label_set (s_Title, i2);
	}
}

void sidecar_wgview_set (const sidecar_wg_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	int texts = v->kind != s_V.kind || v->voice != s_V.voice || strcmp (v->name, s_V.name) || v->live != s_V.live ||
		    v->frozen != s_V.frozen || strcmp (v->anim_text, s_V.anim_text) || strcmp (v->hold_text, s_V.hold_text) ||
		    v->frames != s_V.frames || v->n != s_V.n || strcmp (v->note, s_V.note) ||
		    v->ms_nz != s_V.ms_nz || v->ms_ready != s_V.ms_ready;
	int cwd = strcmp (v->lib_cwd, s_V.lib_cwd), kind = v->kind != s_V.kind;
	s_V = *v;
	s_nGen = v->gen;
	if (kind) list_keys ();
	if (s_bList)
	{
		rows_build ();
		if (s_bLib && cwd) s_nTop = 0;			// another folder
		top_clamp ();
		list_keys ();
		title_update ();
		lv_obj_invalidate (s_List);
	}
	else lv_obj_invalidate (s_Area);
	if (!texts) return;
	char i2[64];
	if (v->kind == WGV_GRANULAR)
	{
		lv_snprintf (i2, sizeof i2, "%s%s  HOLD %s  ANIM %s", v->live ? "LIVE " : "", v->name, v->hold_text, v->anim_text);
		th_label_set (s_Info2, i2);
		lv_snprintf (i2, sizeof i2, "%d GRAINS%s%s%s", v->n, v->frozen ? "  FROZEN" : "", v->note[0] ? "  " : "", v->note);
		th_label_set (s_Info, i2);
	}
	else if (v->kind == WGV_WAVETABLE)
	{
		lv_snprintf (i2, sizeof i2, "%s  %d FRAMES", v->name, v->frames);
		th_label_set (s_Info, i2);
		th_label_set (s_Info2, "");
	}
	else if (v->kind == WGV_MULTISAMPLE)
	{
		if (v->ms_ready) lv_snprintf (i2, sizeof i2, "%s  %d ZONE%s", v->name[0] ? v->name : "-", v->ms_nz, v->ms_nz == 1 ? "" : "S");
		else lv_snprintf (i2, sizeof i2, "%s", v->name[0] ? v->name : "NO INSTRUMENT");
		th_label_set (s_Info, i2);
		lv_snprintf (i2, sizeof i2, "%s", v->note);
		th_label_set (s_Info2, i2);
	}
	else
	{
		th_label_set (s_Info, "TAP A GRAN / WAVE / MULTI VOICE ON HOME");
		th_label_set (s_Info2, "");
	}
	title_update ();
}
