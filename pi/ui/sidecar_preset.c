// pi/ui/sidecar_preset.c - see sidecar_preset.h. One code-drawn list (the POOL page's look: five tabs on top, rows, a scroll
// bar, the keys below), every mark a filled rect through the theme (th_fill), so both themes draw it and the OLED theme
// keeps its dots.
#include "sidecar_preset.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_List, *s_Up, *s_Dn, *s_Mid, *s_Done;
static sidecar_preset_view_t s_V;
static unsigned s_nGen = ~0u;
static void (*s_Cmd) (const char *);
static void (*s_PoolFn) (int);
static int s_nKind, s_nFolder = -1, s_nTop;	// the list shown, its folder (-1: the project's list), its first row
static int s_nAt = -1;				// the index is aimed at this slot (-1: the first empty one)
static int s_bIndex, s_bReplace;		// the index shown (over the list); the slot it fills holds a preset
static int s_nMenu = -1;			// the slot whose menu is open
static int s_nCount[2] = {-1, -1};
static char s_Msg[80];
static unsigned s_nMsgSeq, s_nIgen;
static int s_bSeen;
enum { TABS = 5 };

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_bg (void) { return oled () ? TH->bg : TH->win; }
static const char *kName[2] = {"JUNO", "6-OP FM"};
static const char *kTag[2] = {"juno", "fm"};

static int row_h (void) { return oled () ? LU (18) : LU (31); }
static int bar_w (void) { return oled () ? LU (5) : LU (8); }
static int hdr (void) { return s_bIndex ? 2 : 1; }			// the tab row, in the index the path row too
static int nvis (void) { return lv_obj_get_height (s_List) / row_h (); }
static int rows_shown (void) { int n = nvis () - hdr () - (s_Msg[0] ? 1 : 0); n = n < 1 ? 1 : n; return n > PRE_ROWS ? PRE_ROWS : n; }
static int rows_ok (void)
{
	return s_V.ready && s_V.kind == s_nKind && s_V.folder == s_nFolder && s_V.first == s_nTop;
}
static void send (const char *c) { if (s_Cmd) s_Cmd (c); }
static void say (const char *t) { lv_snprintf (s_Msg, sizeof s_Msg, "%s", t); }
static int total (void) { return s_V.kind == s_nKind && s_V.folder == s_nFolder ? s_V.total : 0; }

static void top_clamp (void)
{
	int n = rows_shown (), t = total ();
	if (s_nTop > t - n) s_nTop = t - n;
	if (s_nTop < 0) s_nTop = 0;
}

static void refresh (void);
static void keys (void);

static void draw_menu (lv_layer_t *l, const lv_area_t *c, int w, int h)
{
	if (s_nMenu < 0) return;
	int od = oled (), rh = row_h (), m = LU (od ? 8 : 40), x0 = m, bw = w - bar_w () - 3 - 2 * m, n = 3, y0 = (h - n * rh) / 2;
	const lv_font_t *fb = TH->f_body, *fs = od ? TH->f_micro : TH->f_small;
	th_fill (l, c, x0 - 2, y0 - 2, x0 + bw + 1, y0 + n * rh + 1, c_bg (), LV_OPA_COVER, 0);
	th_frame (l, c, x0 - 2, y0 - 2, x0 + bw + 1, y0 + n * rh + 1, c_ink (), 1, 0);
	char t[64];
	int r = s_nMenu - s_nTop;
	lv_snprintf (t, sizeof t, "%03d  %s", s_nMenu, r >= 0 && r < s_V.n ? s_V.name[r] : "");
	th_text (l, c, x0 + LU (4), y0 + (rh - fs->line_height) / 2, bw - LU (8), fs, c_dim (), LV_TEXT_ALIGN_LEFT, t);
	for (int i = 0; i < 2; i++)
	{
		int y = y0 + (i + 1) * rh;
		th_fill (l, c, x0, y, x0 + bw - 1, y, od ? TH->faint : th_mix (c_bg (), c_dim (), 90), LV_OPA_COVER, 0);
		th_text (l, c, x0, y + (rh - fb->line_height) / 2, bw, fb, c_ink (), LV_TEXT_ALIGN_CENTER,
			 i == 0 ? (od ? "REPLACE" : "Replace") : (od ? "CANCEL" : "Cancel"));
	}
}

static void list_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), rh = row_h (), od = oled ();
	const sidecar_preset_view_t *v = &s_V;
	const lv_font_t *fb = TH->f_body, *fs = od ? TH->f_micro : TH->f_small;
	int pad = od ? 3 : 10, bar = bar_w (), rw = w - bar - 3, nh = hdr (), body = rows_shown ();
	uint32_t line = od ? TH->faint : th_mix(c_bg (), c_dim (), 90);
	int ok = rows_ok ();
	// the tab row: the POOL page's three lists, then JUNO and 6-OP FM (the one shown selected), each with its rows
	{
		int cw = rw / TABS;
		for (int b = 0; b < TABS; b++)
		{
			char t[24];
			int on = b == 3 + s_nKind, x0 = b * cw;
			static const char *const kTab[3] = {"SAMPLES", "TABLES", "INSTR"};
			if (b < 3) lv_snprintf (t, sizeof t, "%s", kTab[b]);
			else if (s_nCount[b - 3] >= 0) lv_snprintf (t, sizeof t, "%s %d", b == 3 ? "JUNO" : "6-OP", s_nCount[b - 3]);
			else lv_snprintf (t, sizeof t, "%s", b == 3 ? "JUNO" : "6-OP");
			if (on) th_fill (l, &c, x0 + 1, 1, x0 + cw - 2, rh - 2, TH->sel_bg, LV_OPA_COVER, 0);
			th_text (l, &c, x0, (rh - fs->line_height) / 2, cw, fs, on ? TH->sel_ink : c_dim (), LV_TEXT_ALIGN_CENTER, t);
		}
		th_fill (l, &c, 0, rh - 1, rw, rh - 1, line, LV_OPA_COVER, 0);
	}
	if (s_bIndex)					// the path row: where the index is, what a tap on a preset does; a tap: up
	{
		char t[96];
		const char *path = ok && v->title[0] ? v->title : kName[s_nKind];
		char to[24] = "";
		if (s_nAt >= 0) lv_snprintf (to, sizeof to, "%s %03d", s_bReplace ? "REPLACE" : "->", s_nAt);
		else lv_snprintf (to, sizeof to, "ADD");
		lv_snprintf (t, sizeof t, "%s  %s%s", od ? "<" : LV_SYMBOL_LEFT, path, v->full && v->folder == s_nKind ? "  (FULL)" : "");
		th_text (l, &c, pad, rh + (rh - fb->line_height) / 2, rw - 2 * pad - (od ? LU (70) : LU (120)), fb, c_ink (), LV_TEXT_ALIGN_LEFT, t);
		th_text (l, &c, pad, rh + (rh - fs->line_height) / 2, rw - 2 * pad, fs, TH->accent, LV_TEXT_ALIGN_RIGHT, to);
		th_fill (l, &c, 0, 2 * rh - 1, rw, 2 * rh - 1, line, LV_OPA_COVER, 0);
	}
	int n = ok ? v->n : 0;
	if (!ok || !total ())
	{
		const char *t = !s_bIndex ? "" : v->ready || ok ? (od ? "EMPTY FOLDER" : "Empty folder") : (od ? "READING THE CARD" : "Reading the card");
		if (!ok && !s_bIndex) t = "";
		th_text (l, &c, 0, (nh + 1) * rh + (rh - fb->line_height) / 2, rw, fb, c_dim (), LV_TEXT_ALIGN_CENTER, t);
	}
	for (int i = 0; i < body && i < n; i++)
	{
		int r = s_nTop + i, y = (nh + i) * rh, dir = v->flags[i] & 1, used = v->flags[i] & 2;
		char t[PRE_NAME + 16];
		if (s_bIndex)				// a folder or a preset
		{
			if (dir) lv_snprintf (t, sizeof t, "%s/", v->name[i]);
			else lv_snprintf (t, sizeof t, "%s", v->name[i]);
			th_text (l, &c, pad, y + (rh - fb->line_height) / 2, rw - 2 * pad - (od ? LU (40) : LU (90)), fb, c_ink (), LV_TEXT_ALIGN_LEFT, t);
			th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs, c_dim (), LV_TEXT_ALIGN_RIGHT,
				 dir ? (od ? ">" : LV_SYMBOL_RIGHT) : s_bReplace ? (od ? "REPLACE" : "Replace") : (od ? "ADD" : "Add"));
			th_fill (l, &c, 0, y + rh - 1, rw, y + rh - 1, od ? TH->bg : th_mix (c_bg (), c_dim (), 40), LV_OPA_COVER, 0);
			continue;
		}
		const char *nm = used ? v->name[i] : "---";
		lv_snprintf (t, sizeof t, "%03d  %s", r, nm);
		th_text (l, &c, pad, y + (rh - fb->line_height) / 2, rw - 2 * pad - (od ? LU (60) : LU (120)), fb, used ? c_ink () : c_dim (),
			 LV_TEXT_ALIGN_LEFT, t);
		if (v->built_in) th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs, c_dim (), LV_TEXT_ALIGN_RIGHT, od ? "BUILT-IN" : "Built-in");
		th_fill (l, &c, 0, y + rh - 1, rw, y + rh - 1, od ? TH->bg : th_mix (c_bg (), c_dim (), 40), LV_OPA_COVER, 0);
	}
	if (s_Msg[0])					// the message row, above the keys
	{
		int y = (nvis () - 1) * rh;
		th_fill (l, &c, 0, y + 1, rw, y + rh - 1, od ? TH->faint : th_mix (c_bg (), TH->warn, 40), LV_OPA_COVER, 0);
		th_text (l, &c, pad, y + (rh - fs->line_height) / 2, rw - 2 * pad, fs, od ? TH->ink : TH->warn, LV_TEXT_ALIGN_LEFT, s_Msg);
	}
	int y0 = nh * rh, sh = body * rh, tot = total ();
	th_fill (l, &c, w - bar, y0, w - 1, y0 + sh - 1, od ? TH->faint : th_mix (c_bg (), c_dim (), 60), LV_OPA_COVER, 0);
	if (tot > body)
	{
		int th = sh * body / tot < 6 ? 6 : sh * body / tot, ty = y0 + (sh - th) * s_nTop / (tot - body);
		th_fill (l, &c, w - bar, ty, w - 1, ty + th - 1, c_ink (), LV_OPA_COVER, 0);
	}
	else th_fill (l, &c, w - bar, y0, w - 1, y0 + sh - 1, c_dim (), LV_OPA_COVER, 0);
	draw_menu (l, &c, w, h);
}

static void title_update (void)
{
	char t[64], r[48];
	lv_snprintf (t, sizeof t, "POOL  %s", s_bIndex ? "PRESET INDEX" : kName[s_nKind]);
	if (s_V.kind == s_nKind && s_V.folder == s_nFolder && s_V.ready)
	{
		if (s_bIndex) lv_snprintf (r, sizeof r, "%s  %d ROWS", kName[s_nKind], s_V.total);
		else lv_snprintf (r, sizeof r, "%s", s_V.built_in ? "BUILT-IN: ADD MAKES THE LIST" : "THE PROJECT'S LIST");
	}
	else r[0] = 0;
	if (s_TitleBar) th_titlebar_set (s_TitleBar, t, r, 0);
	else
	{
		char i2[120];
		lv_snprintf (i2, sizeof i2, "%s   %s", t, r);
		th_label_set (s_Title, i2);
	}
}

static void keys (void)
{
	th_key_text (s_Mid, "ADD");
	if (s_bIndex) lv_obj_add_flag (s_Mid, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Mid, LV_OBJ_FLAG_HIDDEN);
	th_key_text (s_Done, s_bIndex ? "BACK" : "DONE");
}

static void refresh (void) { top_clamp (); keys (); title_update (); lv_obj_invalidate (s_List); }

static void index_open (int at, int replace)	// the index at its root, aimed at a slot (-1: the first empty one)
{
	s_bIndex = 1, s_nAt = at, s_bReplace = replace, s_nFolder = s_nKind, s_nTop = 0, s_nMenu = -1, s_Msg[0] = 0;
	s_nIgen = 0;
	refresh ();
}
static void list_open (void)
{
	s_bIndex = 0, s_nAt = -1, s_bReplace = 0, s_nFolder = -1, s_nTop = 0, s_nMenu = -1;
	refresh ();
}

static void preset_tap (int i)			// a preset of the index
{
	char cmd[96];
	if (!rows_ok () || i < 0 || i >= s_V.n) return;
	if (s_nAt >= 0)
		lv_snprintf (cmd, sizeof cmd, "preset=%s:set:%u:%d:%d:%d", kTag[s_nKind], s_V.igen, s_nFolder, s_nTop + i, s_nAt);
	else lv_snprintf (cmd, sizeof cmd, "preset=%s:add:%u:%d:%d", kTag[s_nKind], s_V.igen, s_nFolder, s_nTop + i);
	send (cmd);
	say ("SENT");
	if (s_nAt >= 0) list_open ();		// a slot named: back to the rows, with the answer in the message row
	refresh ();
}

static void list_tap (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int x = p.x - c.x1, y = p.y - c.y1, rh = row_h (), i = y / rh, w = lv_area_get_width (&c), nh = hdr ();
	if (s_nMenu >= 0)				// the menu: REPLACE, CANCEL, or outside it: closed
	{
		int m = LU (oled () ? 8 : 40), bw = w - bar_w () - 3 - 2 * m, y0 = (lv_area_get_height (&c) - 3 * rh) / 2;
		int bi = y >= y0 && x >= m && x < m + bw ? (y - y0) / rh : -1;
		if (bi == 1) index_open (s_nMenu, 1);
		else s_nMenu = -1;
		refresh ();
		return;
	}
	s_Msg[0] = 0;
	if (i == 0)					// the tab row
	{
		int rw = w - bar_w () - 3, cw = rw / TABS, b = x / (cw > 0 ? cw : 1);
		b = b < 0 ? 0 : b >= TABS ? TABS - 1 : b;
		if (b < 3) { if (s_PoolFn) s_PoolFn (b); return; }
		s_nKind = b - 3;
		list_open ();
		return;
	}
	if (s_bIndex && i == 1)				// the path row: up a folder, at a root back to the list
	{
		if (rows_ok () && s_V.up >= 0) s_nFolder = s_V.up, s_nTop = 0;
		else list_open ();
		refresh ();
		return;
	}
	if (i < nh || i >= nh + rows_shown ()) { refresh (); return; }
	int k = i - nh;
	if (!rows_ok () || k >= s_V.n) { refresh (); return; }
	if (s_bIndex)
	{
		if (s_V.flags[k] & 1)
		{
			if (s_V.kid[k] >= 0) s_nFolder = s_V.kid[k], s_nTop = 0;
		}
		else preset_tap (k);
		refresh ();
		return;
	}
	if (s_V.flags[k] & 2) s_nMenu = s_nTop + k;	// a filled slot: its menu
	else index_open (s_nTop + k, 0);		// an empty one: the index aimed at it
	refresh ();
}

static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	s_nMenu = -1;
	if (k == 3)					// DONE / BACK
	{
		s_Msg[0] = 0;
		if (s_bIndex) { list_open (); return; }
		if (s_PoolFn) s_PoolFn (-1);
		return;
	}
	if (k == 4) { index_open (-1, 0); return; }	// ADD
	s_nTop += (k == 1 ? -1 : 1) * (rows_shown () - 1);
	top_clamp ();
	lv_obj_invalidate (s_List);
}

void sidecar_preset_create (void *parent, int y0)
{
	int o = oled ();
	s_nGen = ~0u;
	memset (&s_V, 0, sizeof s_V);
	s_V.folder = -2;
	const int dx = TH->dx, dy = TH->dy, q = dx / 4;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, th_page_w (), o ? TH->h : LU (428) + dy);
	s_Page = pg;
	if (o)
	{
		const int ky = LU (204) + dy;
		s_TitleBar = th_titlebar (pg, 0, 0, LU (352) + dx, LU (13));
		th_titlebar_set (s_TitleBar, "POOL", "", 0);
		s_Title = 0;
		s_List = th_box (pg, 0, LU (16), LU (352) + dx, LU (180) + dy);
		s_Up = th_key (pg, 0, ky, LU (84) + q, LU (34), "UP", TH_KEY | TH_KEY_SMALL, key_cb, (void *) 1);
		s_Dn = th_key (pg, LU (90) + q, ky, LU (84) + q, LU (34), "DOWN", TH_KEY | TH_KEY_SMALL, key_cb, (void *) 2);
		s_Mid = th_key (pg, LU (180) + 2 * q, ky, LU (78) + q, LU (34), "ADD", TH_KEY_FUNC | TH_KEY_SMALL, key_cb, (void *) 4);
		s_Done = th_key (pg, LU (264) + 3 * q, ky, LU (88) + q, LU (34), "DONE", TH_KEY_ACCENT | TH_KEY_SMALL, key_cb, (void *) 3);
	}
	else
	{
		const int ky = LU (372) + dy;
		s_TitleBar = 0;
		th_panel (pg, LU (8), LU (6), LU (784) + dx, LU (360) + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, LU (24), LU (14), TH->f_mid, TH->win_ink, "POOL");
		s_List = th_box (pg, LU (24), LU (44), LU (752) + dx, LU (310) + dy);
		s_Up = th_key (pg, LU (16), ky, LU (140) + q, LU (52), "UP", TH_KEY, key_cb, (void *) 1);
		s_Dn = th_key (pg, LU (164) + q, ky, LU (140) + q, LU (52), "DOWN", TH_KEY, key_cb, (void *) 2);
		s_Mid = th_key (pg, LU (312) + 2 * q, ky, LU (296) + q, LU (52), "ADD", TH_KEY_FUNC, key_cb, (void *) 4);
		s_Done = th_key (pg, LU (616) + 3 * q, ky, LU (176) + q, LU (52), "DONE", TH_KEY_ACCENT, key_cb, (void *) 3);
	}
	lv_obj_add_event_cb (s_List, list_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_flag (s_List, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_List, list_tap, LV_EVENT_CLICKED, 0);
	s_bIndex = 0, s_nFolder = -1, s_nTop = 0, s_nAt = -1, s_nMenu = -1, s_Msg[0] = 0, s_nMsgSeq = 0, s_nIgen = 0, s_bSeen = 0;
	keys ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_preset_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}
int sidecar_preset_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
void sidecar_preset_open (int kind)
{
	s_nKind = kind ? 1 : 0;
	if (!s_Page) return;
	list_open ();
}
void sidecar_preset_set (const sidecar_preset_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	int fresh = v->msg_seq != s_nMsgSeq;
	if (v->count[0] >= 0 || v->count[1] >= 0) s_nCount[0] = v->count[0], s_nCount[1] = v->count[1];
	s_V = *v;
	s_nGen = v->gen;
	if (fresh)					// an add ended (here, on the web page or on the Rytm): its word
	{
		if (s_bSeen) say (v->msg);
		s_nMsgSeq = v->msg_seq;
	}
	s_bSeen = 1;
	if (s_bIndex && v->ready && v->kind == s_nKind && v->folder == s_nFolder && s_nFolder >= 0)
	{
		if (!s_nIgen) s_nIgen = v->igen;
		else if (v->igen != s_nIgen)			// the card changed and the index was walked again: its folder numbers are new
		{
			s_nIgen = v->igen, s_nFolder = s_nKind, s_nTop = 0;
			say ("THE CARD CHANGED: THE INDEX IS NEW");
		}
	}
	refresh ();
}
int sidecar_preset_asking (void) { return s_nMenu >= 0; }
int sidecar_preset_index_shown (void) { return s_bIndex; }
int sidecar_preset_kind (void) { return s_nKind; }
int sidecar_preset_folder (void) { return s_nFolder; }
int sidecar_preset_first (void) { return s_nTop; }
int sidecar_preset_count (int kind) { return kind >= 0 && kind < 2 ? s_nCount[kind] : -1; }
void sidecar_preset_set_command (void (*fn) (const char *cmd)) { s_Cmd = fn; }
void sidecar_preset_set_pool (void (*fn) (int list)) { s_PoolFn = fn; }
