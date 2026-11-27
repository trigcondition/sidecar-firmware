// pi/ui/sidecar_morph.c - see sidecar_morph.h. The left half the A / B keys over the twelve pads (two rows of six), the
// right half the selected scene's locks (a row: name, a slider, the value, DEL; UP / DN scroll), the crossfader across
// the bottom with A's scene on its left and B's on its right. ADD (0.5.0 v2) puts a picker in the list's place: the
// track (RT1..RT12, RFX, V1..V8), its page, the param, the value, ADD / BACK - the Rytm's MORPH page's ADD row's tables
// (core/ui/page_morph.cpp). No fixed places: from the page's width and height.
#include "sidecar_morph.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

#define LED_A	47	// the Rytm's palette (core/ui/extra.h kLedA / kLedB): A blue, B orange
#define LED_B	52

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_A, *s_B, *s_Pad[SMO_SCENES], *s_ListT, *s_Up, *s_Dn, *s_Name[SMO_ROWS],
	*s_Sl[SMO_ROWS], *s_Val[SMO_ROWS], *s_Del[SMO_ROWS], *s_FadA, *s_FadB, *s_Fader, *s_Info;
static lv_obj_t *s_AddK, *s_Add, *s_AddT, *s_AddL[3], *s_AddV, *s_AddS, *s_AddGo;
static int s_bAdd, s_nAddTr, s_nAddPg, s_nAddPr, s_nAddV = 64;
static lv_obj_t *s_Undo;	// DEL's undo (v2 Task 7): the removed lock, for 2 s
static char s_UndoName[16], s_UndoLabel[16], s_UndoVal[8];
static int s_nUndoScene = -1;
static uint32_t s_nUndoMs;
static char s_AddName[16];
static sidecar_morph_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_morph_command_t s_Cmd;
static int s_nRows;		// the list's rows that fit
static int s_nTop;		// the scroll the page asks for
static int s_nAssign;		// 1: the next pad becomes A, 2: B
static int s_nSel = -2;		// the scene the list showed (another: the list from its top)

static int oled (void) { return TH->id == THEME_OLED; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

// ---- the ADD picker's tables (core/ui/page_morph.cpp's ADD row; core/mod/kinds.cpp's names)
#define ADD_TRACKS	21	// RT1..RT12, RFX, V1..V8
typedef struct { const char *name; const char *const *p; int n; } add_page_t;
static const char *const kSrc[] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
static const char *const kSmpl[] = {"TUN", "FIN", "SMP", "BR", "STA", "END", "LOP", "LEV"};
static const char *const kFltr[] = {"FENV", "FATK", "FDEC", "FSUS", "FREL", "FRQ", "RES", "FTYP"};
static const char *const kAmp[] = {"ATK", "HLD", "DEC", "OVR", "VOL", "PAN", "ACC", "DEL", "REV"};
static const char *const kLfo[] = {"MUL", "WAV", "MOD", "SPD", "FAD", "SPH", "DEP"};
static const char *const kDly[] = {"TIM", "X", "WID", "FDB", "DHPF", "DLPF", "DREV", "DVOL", "DOV"};
static const char *const kRev[] = {"PRE", "RDEC", "RFRQ", "GAI", "RHPF", "RLPF", "RVOL"};
static const char *const kDst[] = {"AMT", "SYM"};
static const char *const kCmp[] = {"THR", "CATK", "CREL", "RAT", "SEQ", "MUP", "MIX", "CVOL"};
static const char *const kFxLfo[] = {"SPD", "FAD", "DEP"};
static const char *const kP1[] = {"P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
static const char *const kP9[] = {"P9", "P10", "P11", "P12", "P13", "P14", "P15", "P16"};
static const add_page_t kRtPg[] = {{"SRC", kSrc, 8}, {"SMPL", kSmpl, 8}, {"FLTR", kFltr, 8}, {"AMP", kAmp, 9}, {"LFO", kLfo, 7}};
static const add_page_t kFxPg[] = {{"DELAY", kDly, 9}, {"REVERB", kRev, 7}, {"DIST", kDst, 2}, {"COMP", kCmp, 8}, {"LFO", kFxLfo, 3}};
static const add_page_t kVPg[] = {{"P1-8", kP1, 8}, {"P9-16", kP9, 8}};
static const add_page_t *add_pages (int tr, int *n)
{
	if (tr < 12) { *n = 5; return kRtPg; }
	if (tr == 12) { *n = 5; return kFxPg; }
	*n = 2; return kVPg;
}
static void add_track_name (int tr, char *b, int cap)
{
	if (tr < 12) lv_snprintf (b, cap, "RT%d", tr + 1);
	else if (tr == 12) lv_snprintf (b, cap, "RFX");
	else lv_snprintf (b, cap, "V%d", tr - 12);
}
static void add_refresh (void)
{
	int np;
	const add_page_t *pg = add_pages (s_nAddTr, &np);
	if (s_nAddPg >= np) s_nAddPg = 0;
	if (s_nAddPr >= pg[s_nAddPg].n) s_nAddPr = 0;
	char tr[8], t[40];
	add_track_name (s_nAddTr, tr, sizeof tr);
	lv_snprintf (s_AddName, sizeof s_AddName, "%s:%s", tr, pg[s_nAddPg].p[s_nAddPr]);
	if (!s_Add) return;
	lv_snprintf (t, sizeof t, "S%d ADD %s", s_V.sel + 1, s_AddName);
	th_label_set (s_AddT, t);
	th_label_set (s_AddL[0], tr);
	th_label_set (s_AddL[1], pg[s_nAddPg].name);
	th_label_set (s_AddL[2], pg[s_nAddPg].p[s_nAddPr]);
	lv_snprintf (t, sizeof t, "%d", s_nAddV);
	th_label_set (s_AddV, t);
	if (!th_slider_dragging (s_AddS)) th_slider_set (s_AddS, s_nAddV);
	th_key_enable (s_AddGo, s_V.sel >= 0 && s_V.nlocks[s_V.sel] < 64);
}
static void list_shown (int on);
static void refresh (void);
void sidecar_morph_add (int on)
{
	s_bAdd = !!on;
	if (on) add_refresh ();
	else s_AddName[0] = 0;
	if (s_Add) list_shown (!on), refresh ();
}
void sidecar_morph_add_step (int field, int d)
{
	int np;
	if (field == 0) s_nAddTr = (s_nAddTr + d + ADD_TRACKS) % ADD_TRACKS, s_nAddPg = s_nAddPr = 0;
	else if (field == 1) { add_pages (s_nAddTr, &np); s_nAddPg = (s_nAddPg + d + np) % np, s_nAddPr = 0; }
	else { const add_page_t *pg = add_pages (s_nAddTr, &np); s_nAddPr = (s_nAddPr + d + pg[s_nAddPg].n) % pg[s_nAddPg].n; }
	add_refresh ();
}
void sidecar_morph_add_value (int v) { s_nAddV = v < 0 ? 0 : v > 127 ? 127 : v; add_refresh (); }
void sidecar_morph_add_go (void)
{
	if (!s_bAdd || s_V.sel < 0) return;
	add_refresh ();
	char c[48];
	lv_snprintf (c, sizeof c, "scene=lock:%d:%s:%d", s_V.sel + 1, s_AddName, s_nAddV);
	cmd (c);
}
const char *sidecar_morph_add_name (void) { return s_bAdd ? s_AddName : ""; }
static void addk_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; sidecar_morph_add (!s_bAdd); }
static void adds_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; int w = (int) (intptr_t) ud; sidecar_morph_add_step (w / 2, w & 1 ? 1 : -1); }
static void addv_cb (lv_obj_t *o, int v, void *ud) { (void) o; (void) ud; s_nAddV = v & 0xFFFF; add_refresh (); }
static void addgo_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	if ((int) (intptr_t) ud) sidecar_morph_add_go ();
	else sidecar_morph_add (0);
}

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
static void refresh (void);
static void undo_show (int on)
{
	if (!s_Undo) return;
	if (on) lv_obj_remove_flag (s_Undo, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Undo, LV_OBJ_FLAG_HIDDEN);
}
static void del_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int r = (int) (intptr_t) ud;
	if (r >= s_V.nrows || s_V.sel < 0) return;
	const sidecar_morph_row_t *x = &s_V.row[r];
	lv_snprintf (s_UndoName, sizeof s_UndoName, "%s", x->name);
	lv_snprintf (s_UndoLabel, sizeof s_UndoLabel, "%s", x->label[0] ? x->label : x->name);
	lv_snprintf (s_UndoVal, sizeof s_UndoVal, "%s", x->value);
	s_nUndoScene = s_V.sel, s_nUndoMs = lv_tick_get () | 1;
	char c[48];
	lv_snprintf (c, sizeof c, "scene=unlock:%d:%s", s_V.sel + 1, x->name);
	cmd (c);
	undo_show (1);
	refresh ();
}
static void undo_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	if (!s_nUndoMs || s_nUndoScene < 0) return;
	char c[48];
	lv_snprintf (c, sizeof c, "scene=lock:%d:%s:%s", s_nUndoScene + 1, s_UndoName, s_UndoVal);
	s_nUndoMs = 0;
	undo_show (0);
	cmd (c);
	refresh ();
}
void sidecar_morph_del (int row) { if (row >= 0 && row < s_nRows) del_cb (0, 0, (void *) (intptr_t) row); }
void sidecar_morph_undo (void) { undo_cb (0, 0, 0); }
int sidecar_morph_undo_shown (void) { return s_nUndoMs != 0; }
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
	// live while dragging (absolute: spec-050 2.4), at most every 30 ms (each line is a console command on core 3),
	// and always the value the finger lifts at
	static uint32_t s_nAt;
	uint32_t now = lv_tick_get ();
	if (!(v & TH_RELEASED) && now - s_nAt < 30) return;
	s_nAt = now;
	char c[24];
	lv_snprintf (c, sizeof c, "scene=fader:%d", v & 0xFFFF);
	cmd (c);
}

void sidecar_morph_create (void *parent, int y0)
{
	const int o = oled ();
	s_nGen = ~0u, s_nTop = 0, s_nAssign = 0, s_nSel = -2, s_bAdd = 0, s_Add = 0;
	s_nAddTr = s_nAddPg = s_nAddPr = 0, s_nAddV = 64;
	s_nUndoMs = 0, s_nUndoScene = -1, s_Undo = 0;
	memset (&s_V, 0, sizeof s_V);
	const int pw = o ? TH->w - 48 : th_page_w (), ph = TH->h - y0 - (o ? LU (25) : 0);
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph + (o ? LU (25) : 0));
	s_Page = pg;
	const lv_font_t *f = o ? TH->f_micro : TH->f_small;
	const int kind = o ? TH_KEY | TH_KEY_SMALL : TH_KEY, ink = o ? TH->ink : TH->win_ink, dim = o ? TH->dim : TH->win_dim;
	const int x0 = o ? 0 : LU (24), w = o ? pw : pw - LU (48), top = o ? LU (16) : LU (44), keyh = o ? LU (20) + TH->pdy / 12 : LU (38) + TH->pdy / 8;
	const int foot = o ? LU (18) : LU (44), fh = o ? LU (14) + TH->pdy / 16 : LU (34) + TH->pdy / 8;	// the hint line; the fader's height
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, LU (13));
		th_titlebar_set (s_TitleBar, "MORPH", "", 0);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, LU (8), LU (6), pw - LU (16), ph - LU (14), "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, LU (24), LU (14), TH->f_mid, TH->win_ink, "MORPH");
	}
	// the left half: A / B, then the pads in two rows of six
	const int lw = w * 9 / 20, gap = LU (o ? 3 : 6), pwid = (lw - 5 * gap) / 6;
	s_A = th_key (pg, x0, top, (lw - gap) / 2, keyh, "A", kind, ab_cb, (void *) (intptr_t) 1);
	s_B = th_key (pg, x0 + (lw + gap) / 2, top, (lw - gap) / 2, keyh, "B", kind, ab_cb, (void *) (intptr_t) 2);
	const int fy = ph - (o ? 0 : LU (14)) - foot - fh, py = top + keyh + gap * 2, padh = (fy - gap * 3 - py) / 2;
	for (int n = 0; n < SMO_SCENES; n++)
	{
		char t[4];
		lv_snprintf (t, sizeof t, "%d", n + 1);
		s_Pad[n] = th_key (pg, x0 + (n % 6) * (pwid + gap), py + (n / 6) * (padh + gap), pwid, padh, t, kind, pad_cb,
				   (void *) (intptr_t) n);
	}
	// the right half: the list
	const int rx = x0 + lw + gap * 3, rw = w - lw - gap * 3, kw = LU (o ? 26 : 44);
	s_ListT = th_label (pg, rx, top, f, dim, "");
	lv_obj_set_width (s_ListT, rw - 3 * kw - 3 * gap);
	s_AddK = th_key (pg, rx + rw - 3 * kw - 2 * gap, top, kw, keyh * 2 / 3, "ADD", kind, addk_cb, 0);
	s_Up = th_key (pg, rx + rw - 2 * kw - gap, top, kw, keyh * 2 / 3, "UP", kind, scroll_cb, (void *) (intptr_t) -1);
	s_Dn = th_key (pg, rx + rw - kw, top, kw, keyh * 2 / 3, "DN", kind, scroll_cb, (void *) (intptr_t) 1);
	const int ly = top + keyh * 2 / 3 + gap, rh = o ? LU (16) + TH->pdy / 20 : LU (30) + TH->pdy / 10;
	s_nRows = (fy - gap * 2 - ly) / rh;
	if (s_nRows > SMO_ROWS) s_nRows = SMO_ROWS;
	const int nw = rw * 2 / 5, vw = LU (o ? 30 : 48), dw = LU (o ? 28 : 46), sw = rw - nw - vw - dw - 3 * gap;
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
	// ADD's picker in the list's place: TRACK / PAGE / PARAM (- name +), the value's slider, ADD / BACK
	{
		const int ah = fy - gap * 2 - ly, arh = ah / 6, sk = o ? LU (24) : LU (48), lw3 = o ? 34 : 70;
		s_Add = th_box (pg, rx, ly, rw, ah);
		s_AddT = th_label (s_Add, 0, 0, f, ink, "");
		lv_obj_set_width (s_AddT, rw);
		static const char *const cap[3] = {"TRACK", "PAGE", "PARAM"};
		for (int r = 0; r < 3; r++)
		{
			int y = (r + 1) * arh;
			th_label (s_Add, 0, y + (arh - lv_font_get_line_height (f)) / 2, f, dim, cap[r]);
			th_key (s_Add, lw3, y + 1, sk, arh - 2, "-", kind, adds_cb, (void *) (intptr_t) (2 * r));
			s_AddL[r] = th_label (s_Add, lw3 + sk + gap, y + (arh - lv_font_get_line_height (f)) / 2, f, ink, "");
			lv_obj_set_width (s_AddL[r], rw - lw3 - 2 * sk - 2 * gap);
			lv_obj_set_style_text_align (s_AddL[r], LV_TEXT_ALIGN_CENTER, 0);
			th_key (s_Add, rw - sk, y + 1, sk, arh - 2, "+", kind, adds_cb, (void *) (intptr_t) (2 * r + 1));
		}
		th_label (s_Add, 0, 4 * arh + (arh - lv_font_get_line_height (f)) / 2, f, dim, "VALUE");
		s_AddS = th_slider (s_Add, lw3, 4 * arh + 2, rw - lw3 - vw - gap, arh - 4, 0, 127, addv_cb, 0);
		s_AddV = th_label (s_Add, rw - vw, 4 * arh + (arh - lv_font_get_line_height (f)) / 2, f, ink, "");
		lv_obj_set_width (s_AddV, vw);
		const int bw = (rw - gap) / 2;
		s_AddGo = th_key (s_Add, 0, 5 * arh + 1, bw, arh - 2, "ADD", o ? kind : TH_KEY_ACCENT, addgo_cb, (void *) (intptr_t) 1);
		th_key (s_Add, bw + gap, 5 * arh + 1, bw, arh - 2, "BACK", kind, addgo_cb, (void *) (intptr_t) 0);
		lv_obj_add_flag (s_Add, LV_OBJ_FLAG_HIDDEN);
	}
	// the crossfader: A's scene left, B's right
	const int lab = LU (o ? 26 : 48);
	s_FadA = th_label (pg, x0, fy + (fh - lv_font_get_line_height (f)) / 2, f, ink, "A -");
	s_FadB = th_label (pg, x0 + w - lab, fy + (fh - lv_font_get_line_height (f)) / 2, f, ink, "B -");
	lv_obj_set_width (s_FadA, lab), lv_obj_set_width (s_FadB, lab);
	lv_obj_set_style_text_align (s_FadB, LV_TEXT_ALIGN_RIGHT, 0);
	s_Fader = th_slider (pg, x0 + lab + gap, fy, w - 2 * (lab + gap), fh, 0, 16383, fader_cb, 0);
	th_slider_default (s_Fader, 0);	// a double tap: A (the scene crossfade off); the lock rows' targets have no default to go back to (DEL removes a lock)
	s_Info = th_label (pg, x0, ph - foot - (o ? 0 : LU (8)) + LU (o ? 4 : 10), f, o ? TH->faint : dim, "");
	lv_obj_set_width (s_Info, w);
	const int uw = LU (o ? 36 : 88), uh = LU (o ? 15 : 34);	// UNDO at the hint line's right end, while offered
	s_Undo = th_key (pg, x0 + w - uw, ph - foot - (o ? 0 : 8) + (o ? 2 : 4), uw, uh, "UNDO", kind, undo_cb, 0);
	lv_obj_add_flag (s_Undo, LV_OBJ_FLAG_HIDDEN);
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
void *sidecar_morph_fader (void) { return s_Fader; }

static void list_shown (int on)	// the lock list's rows, or (ADD) the picker in their place
{
	for (int r = 0; r < s_nRows; r++)
	{
		lv_obj_t *w[4] = {s_Name[r], s_Sl[r], s_Val[r], s_Del[r]};
		for (int k = 0; k < 4; k++) if (on) lv_obj_remove_flag (w[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (w[k], LV_OBJ_FLAG_HIDDEN);
	}
	if (on) lv_obj_remove_flag (s_Up, LV_OBJ_FLAG_HIDDEN), lv_obj_remove_flag (s_Dn, LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag (s_Add, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_add_flag (s_Up, LV_OBJ_FLAG_HIDDEN), lv_obj_add_flag (s_Dn, LV_OBJ_FLAG_HIDDEN), lv_obj_remove_flag (s_Add, LV_OBJ_FLAG_HIDDEN);
	th_key_check (s_AddK, !on);
}

static void refresh (void)
{
	const sidecar_morph_view_t *v = &s_V;
	char t[64], r[32];
	int n = v->sel >= 0 ? v->nlocks[v->sel] : 0;
	if (v->sel != s_nSel) s_nSel = v->sel, s_nTop = 0;
	if (s_nTop > 0 && s_nTop >= n) s_nTop = n > s_nRows ? n - s_nRows : 0;	// the scene has fewer locks now
	const char *ed = v->edit == 1 ? "EDIT A" : v->edit == 2 ? "EDIT B" : "";
	if (v->a >= 0 && v->b >= 0) lv_snprintf (r, sizeof r, "A%d  B%d%s%s", v->a + 1, v->b + 1, ed[0] ? "  " : "", ed);
	else lv_snprintf (r, sizeof r, "%s", ed);
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
	for (int i = 0; i < s_nRows && !s_bAdd; i++)
	{
		int on = i < v->nrows;
		if (on && v->row[i].stale)	// the target no longer resolves: its name with "?", faint (as the Rytm's "?")
		{
			lv_snprintf (t, sizeof t, "%s?", v->row[i].name);
			th_label_set (s_Name[i], t);
		}
		else th_label_set (s_Name[i], on ? (v->row[i].label[0] ? v->row[i].label : v->row[i].name) : "");
		th_label_color (s_Name[i], on && v->row[i].stale ? TH->faint : oled () ? TH->ink : TH->win_ink);
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
	if (s_bAdd) add_refresh ();
	if (s_nUndoMs) lv_snprintf (t, sizeof t, "REMOVED %s", s_UndoLabel);
	th_label_set (s_Info, s_nUndoMs ? t : !v->valid ? "WAITING FOR THE ENGINE"
		: v->locked ? "SCENES LOCKED: THE FILE IS FROM A NEWER BUILD, NOT SAVED"
		: !n ? "NO LOCKS: ADD ONE, OR HOLD PAGE + T15 / T16 AND TURN A KNOB"
		: "TAP A OR B, THEN A PAD: THAT END OF THE FADER");
}

void sidecar_morph_set (const sidecar_morph_view_t *v)
{
	if (s_nUndoMs && lv_tick_elaps (s_nUndoMs) >= 2000)	// the undo's 2 s are over
	{
		s_nUndoMs = 0;
		undo_show (0);
		if (s_Page) refresh ();
	}
	if (!s_Page || v->gen == s_nGen) return;
	s_V = *v;
	s_nGen = v->gen;
	refresh ();
}
