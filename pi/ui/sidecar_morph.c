// pi/ui/sidecar_morph.c - see sidecar_morph.h. The left half the A / B keys over the twelve pads (two rows of six), the
// right half the selected scene's locks (a row: name, a slider, the value, DEL; UP / DN scroll), the crossfader across
// the bottom with A's scene on its left and B's on its right. No fixed places: from the page's width and height.
#include "sidecar_morph.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

#define LED_A	47	// the Rytm's palette (core/ui/extra.h kLedA / kLedB): A blue, B orange
#define LED_B	52

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_A, *s_B, *s_Pad[SMO_SCENES], *s_ListT, *s_Up, *s_Dn, *s_Name[SMO_ROWS],
	*s_Sl[SMO_ROWS], *s_Val[SMO_ROWS], *s_Del[SMO_ROWS], *s_FadA, *s_FadB, *s_Fader, *s_Info;
static sidecar_morph_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_morph_command_t s_Cmd;
static int s_nRows;		// the list's rows that fit
static int s_nTop;		// the scroll the page asks for
static int s_nAssign;		// 1: the next pad becomes A, 2: B
static int s_nSel = -2;		// the scene the list showed (another: the list from its top)

static int oled (void) { return TH->id == THEME_OLED; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

static void ab_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int w = (int) (intptr_t) ud;
	s_nAssign = s_nAssign == w ? 0 : w;
	th_key_check (s_A, s_nAssign == 1);
	th_key_check (s_B, s_nAssign == 2);
}
static void pad_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int n = (int) (intptr_t) ud;
	char c[32];
	if (s_nAssign)	// A or B tapped first: that end (scene=ab: 1-based, 0 none)
	{
		int a = s_nAssign == 1 ? n : s_V.a, b = s_nAssign == 2 ? n : s_V.b;
		lv_snprintf (c, sizeof c, "scene=ab:%d:%d", a + 1, b + 1);
		s_nAssign = 0;
		th_key_check (s_A, 0);
		th_key_check (s_B, 0);
	}
	else lv_snprintf (c, sizeof c, "scene=sel:%d", n + 1);
	cmd (c);
}
static void scroll_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int d = (int) (intptr_t) ud, n = s_V.sel >= 0 ? s_V.nlocks[s_V.sel] : 0, max = n > s_nRows ? n - s_nRows : 0;
	s_nTop += d * s_nRows;
	s_nTop = s_nTop < 0 ? 0 : s_nTop > max ? max : s_nTop;
}
static void del_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int r = (int) (intptr_t) ud;
	if (r >= s_V.nrows || s_V.sel < 0) return;
	char c[48];
	lv_snprintf (c, sizeof c, "scene=unlock:%d:%s", s_V.sel + 1, s_V.row[r].name);
	cmd (c);
}
static void row_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	int r = (int) (intptr_t) ud;
	if (!(v & TH_RELEASED) || r >= s_V.nrows || s_V.sel < 0) return;	// the value goes when the finger lifts
	const sidecar_morph_row_t *x = &s_V.row[r];
	int u = x->lo + ((v & 0xFFFF) * (x->hi - x->lo) + 63) / 127;
	char c[48];
	lv_snprintf (c, sizeof c, "scene=lock:%d:%s:%d", s_V.sel + 1, x->name, u);
	cmd (c);
}
static void fader_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o; (void) ud;
	char c[24];
	lv_snprintf (c, sizeof c, "scene=fader:%d", v & 0xFFFF);	// live while dragging (absolute: spec-050 2.4)
	cmd (c);
}

void sidecar_morph_create (void *parent, int y0)
{
	const int o = oled ();
	s_nGen = ~0u, s_nTop = 0, s_nAssign = 0, s_nSel = -2;
	memset (&s_V, 0, sizeof s_V);
	const int pw = o ? TH->w - 48 : TH->w, ph = TH->h - y0;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph);
	s_Page = pg;
	const lv_font_t *f = o ? TH->f_micro : &lv_font_montserrat_14;
	const int kind = o ? TH_KEY | TH_KEY_SMALL : TH_KEY, ink = o ? TH->ink : TH->win_ink, dim = o ? TH->dim : TH->win_dim;
	const int x0 = o ? 0 : 24, w = o ? pw : pw - 48, top = o ? 16 : 44, keyh = o ? 20 + TH->dy / 12 : 38 + TH->dy / 8;
	const int foot = o ? 18 : 44, fh = o ? 14 + TH->dy / 16 : 34 + TH->dy / 8;	// the hint line; the fader's height
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, 13);
		th_titlebar_set (s_TitleBar, "MORPH", "", 0);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, 8, 6, pw - 16, ph - 14, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "MORPH");
	}
	// the left half: A / B, then the pads in two rows of six
	const int lw = w * 9 / 20, gap = o ? 3 : 6, pwid = (lw - 5 * gap) / 6;
	s_A = th_key (pg, x0, top, (lw - gap) / 2, keyh, "A", kind, ab_cb, (void *) (intptr_t) 1);
	s_B = th_key (pg, x0 + (lw + gap) / 2, top, (lw - gap) / 2, keyh, "B", kind, ab_cb, (void *) (intptr_t) 2);
	const int fy = ph - (o ? 0 : 14) - foot - fh, py = top + keyh + gap * 2, padh = (fy - gap * 3 - py) / 2;
	for (int n = 0; n < SMO_SCENES; n++)
	{
		char t[4];
		lv_snprintf (t, sizeof t, "%d", n + 1);
		s_Pad[n] = th_key (pg, x0 + (n % 6) * (pwid + gap), py + (n / 6) * (padh + gap), pwid, padh, t, kind, pad_cb,
				   (void *) (intptr_t) n);
	}
	// the right half: the list
	const int rx = x0 + lw + gap * 3, rw = w - lw - gap * 3, kw = o ? 26 : 44;
	s_ListT = th_label (pg, rx, top, f, dim, "");
	lv_obj_set_width (s_ListT, rw - 2 * kw - 2 * gap);
	s_Up = th_key (pg, rx + rw - 2 * kw - gap, top, kw, keyh * 2 / 3, "UP", kind, scroll_cb, (void *) (intptr_t) -1);
	s_Dn = th_key (pg, rx + rw - kw, top, kw, keyh * 2 / 3, "DN", kind, scroll_cb, (void *) (intptr_t) 1);
	const int ly = top + keyh * 2 / 3 + gap, rh = o ? 16 + TH->dy / 20 : 30 + TH->dy / 10;
	s_nRows = (fy - gap * 2 - ly) / rh;
	if (s_nRows > SMO_ROWS) s_nRows = SMO_ROWS;
	const int nw = rw * 2 / 5, vw = o ? 30 : 48, dw = o ? 28 : 46, sw = rw - nw - vw - dw - 3 * gap;
	for (int r = 0; r < SMO_ROWS; r++)
	{
		int y = ly + r * rh;
		s_Name[r] = th_label (pg, rx, y + (rh - lv_font_get_line_height (f)) / 2, f, ink, "");
		lv_obj_set_width (s_Name[r], nw);
		s_Sl[r] = th_slider (pg, rx + nw + gap, y + 2, sw, rh - 4, 0, 127, row_cb, (void *) (intptr_t) r);
		s_Val[r] = th_label (pg, rx + nw + sw + 2 * gap, y + (rh - lv_font_get_line_height (f)) / 2, f, ink, "");
		lv_obj_set_width (s_Val[r], vw);
		s_Del[r] = th_key (pg, rx + rw - dw, y + 1, dw, rh - 2, "DEL", kind, del_cb, (void *) (intptr_t) r);
		if (r >= s_nRows)
		{
			lv_obj_add_flag (s_Name[r], LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag (s_Sl[r], LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag (s_Val[r], LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag (s_Del[r], LV_OBJ_FLAG_HIDDEN);
		}
	}
	// the crossfader: A's scene left, B's right
	const int lab = o ? 26 : 48;
	s_FadA = th_label (pg, x0, fy + (fh - lv_font_get_line_height (f)) / 2, f, ink, "A -");
	s_FadB = th_label (pg, x0 + w - lab, fy + (fh - lv_font_get_line_height (f)) / 2, f, ink, "B -");
	lv_obj_set_width (s_FadA, lab), lv_obj_set_width (s_FadB, lab);
	lv_obj_set_style_text_align (s_FadB, LV_TEXT_ALIGN_RIGHT, 0);
	s_Fader = th_slider (pg, x0 + lab + gap, fy, w - 2 * (lab + gap), fh, 0, 16383, fader_cb, 0);
	s_Info = th_label (pg, x0, ph - foot - (o ? 0 : 8) + (o ? 4 : 10), f, o ? TH->faint : dim, "");
	lv_obj_set_width (s_Info, w);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_morph_set_command (sidecar_morph_command_t fn) { s_Cmd = fn; }

void sidecar_morph_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_morph_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

int sidecar_morph_top (void) { return s_nTop; }

static void refresh (void)
{
	const sidecar_morph_view_t *v = &s_V;
	char t[64], r[32];
	int n = v->sel >= 0 ? v->nlocks[v->sel] : 0;
	if (v->sel != s_nSel) s_nSel = v->sel, s_nTop = 0;
	if (s_nTop > 0 && s_nTop >= n) s_nTop = n > s_nRows ? n - s_nRows : 0;	// the scene has fewer locks now
	if (v->a >= 0 && v->b >= 0) lv_snprintf (r, sizeof r, "A%d  B%d%s", v->a + 1, v->b + 1, v->edit ? "  EDIT" : "");
	else lv_snprintf (r, sizeof r, "%s", v->edit ? "EDIT" : "");
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "MORPH", r, 0);
	else { lv_snprintf (t, sizeof t, "MORPH  %s", r); th_label_set (s_Title, t); }
	for (int k = 0; k < SMO_SCENES; k++)
	{
		th_key_check (s_Pad[k], k == v->sel);
		th_key_led (s_Pad[k], k == v->a ? LED_A : k == v->b ? LED_B : 0);
		if (v->nlocks[k]) lv_snprintf (t, sizeof t, "%d", v->nlocks[k]); else t[0] = 0;
		th_key_sub (s_Pad[k], t);
	}
	th_key_led (s_A, LED_A);
	th_key_led (s_B, LED_B);
	if (v->sel >= 0) lv_snprintf (t, sizeof t, "S%d: %d LOCK%s", v->sel + 1, n, n == 1 ? "" : "S");
	else t[0] = 0;
	th_label_set (s_ListT, t);
	th_key_enable (s_Up, s_nTop > 0);
	th_key_enable (s_Dn, s_nTop + s_nRows < n);
	for (int i = 0; i < s_nRows; i++)
	{
		int on = i < v->nrows;
		th_label_set (s_Name[i], on ? v->row[i].name : "");
		th_label_set (s_Val[i], on ? v->row[i].value : "");
		th_slider_set (s_Sl[i], on ? v->row[i].v : 0);
		if (on) lv_obj_remove_flag (s_Sl[i], LV_OBJ_FLAG_HIDDEN), lv_obj_remove_flag (s_Del[i], LV_OBJ_FLAG_HIDDEN);
		else lv_obj_add_flag (s_Sl[i], LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag (s_Del[i], LV_OBJ_FLAG_HIDDEN);
	}
	if (v->a >= 0) lv_snprintf (t, sizeof t, "A%d", v->a + 1); else lv_snprintf (t, sizeof t, "A -");
	th_label_set (s_FadA, t);
	if (v->b >= 0) lv_snprintf (t, sizeof t, "B%d", v->b + 1); else lv_snprintf (t, sizeof t, "B -");
	th_label_set (s_FadB, t);
	if (!th_slider_dragging (s_Fader)) th_slider_set (s_Fader, v->fader);
	th_label_set (s_Info, !v->valid ? "WAITING FOR THE ENGINE"
		: v->locked ? "SCENES LOCKED: THE FILE IS FROM A NEWER BUILD, NOT SAVED"
		: !n ? "NO LOCKS: HOLD A SCENE KEY ON THE RYTM (EDIT ON) AND TURN A KNOB"
		: "TAP A OR B, THEN A PAD: THAT END OF THE FADER");
}

void sidecar_morph_set (const sidecar_morph_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	s_V = *v;
	s_nGen = v->gen;
	refresh ();
}
