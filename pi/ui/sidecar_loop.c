// pi/ui/sidecar_loop.c - see sidecar_loop.h. Top row: the six grab keys and STOP / CUT; the middle: the eight grab pads and
// KEEP; below them BAL and LEV (sliders, absolute) and FADE / MUTE / LIVE (a key each, tap to step); the state line at the
// foot. No fixed places: from the page's width and height.
#include "sidecar_loop.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

#define LED_GREEN	31	// the Rytm's palette (core/ui/page_loop.cpp): playing green, kept blue, STOP red
#define LED_BLUE	47
#define LED_RED		15

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Len[6], *s_Stop, *s_Cut, *s_Pad[SLP_GRABS], *s_Keep, *s_Bal, *s_Lev, *s_BalL,
	*s_LevL, *s_Fade, *s_Mute, *s_Live, *s_State, *s_Info;
static sidecar_loop_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_loop_command_t s_Cmd;
static int s_nSel;		// the grab shown (0 newest), from s_nSelId
static unsigned s_nSelId;	// its id: a grab landing meanwhile moves the list, not what the finger meant

static const char *const kLen[6] = {"1/2", "1", "2", "4", "8", "16"};
static const char *const kFadeT[5] = {"1/4", "1/2", "1", "2", "4"};
static int oled (void) { return TH->id == THEME_OLED; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

static void len_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	char c[24];
	lv_snprintf (c, sizeof c, "loop=grab:%s", (const char *) ud);
	if (c[10] == '1' && c[11] == '/') strcpy (c + 10, "0.5");	// "1/2" is "0.5" on the console
	cmd (c);
}
static void stop_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	cmd (ud ? "loop=stop:cut:now" : "loop=stop");	// STOP: a fade at the next bar line; CUT: 10 ms, at once
}
static void pad_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int n = (int) (intptr_t) ud;
	if (n >= s_V.ngrabs) return;
	s_nSel = n, s_nSelId = s_V.g[n].id;
	char c[24];
	lv_snprintf (c, sizeof c, "loop=play:#%u", s_V.g[n].id);	// shown, and played from the next bar line
	cmd (c);
	s_nGen = ~0u;	// redraw with the new selection
}
static void keep_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	char c[24];
	if (s_nSel >= s_V.ngrabs) return;
	lv_snprintf (c, sizeof c, "loop=keep:#%u", s_V.g[s_nSel].id);
	cmd (c);
}
static void level_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	// absolute, at most every 30 ms while dragging (each line is a console command on core 3), and the value the finger lifts at
	static uint32_t s_nAt[2];
	int w = (int) (intptr_t) ud;
	uint32_t now = lv_tick_get ();
	if (!(v & TH_RELEASED) && now - s_nAt[w] < 30) return;
	s_nAt[w] = now;
	char c[24];
	lv_snprintf (c, sizeof c, "loop=%s:%d", w ? "lev" : "bal", v & 0xFFFF);
	cmd (c);
}
static void fade_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	char c[24];
	lv_snprintf (c, sizeof c, "loop=fade:%s", kFadeT[(s_V.fade + 1) % 5]);
	cmd (c);
}
static void mute_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	cmd (s_V.mute ? "loop=mute:off" : "loop=mute:on");
}
static void live_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	char c[24];
	if (s_V.live >= 12) lv_snprintf (c, sizeof c, "loop=live:off");
	else lv_snprintf (c, sizeof c, "loop=live:perf%d", s_V.live + 1);
	cmd (c);
}

void sidecar_loop_create (void *parent, int y0)
{
	const int o = oled ();
	s_nGen = ~0u, s_nSel = 0, s_nSelId = 0;
	memset (&s_V, 0, sizeof s_V);
	const int pw = o ? TH->w - 48 : th_page_w (), ph = TH->h - y0 - (o ? LU (25) : 0);		// OLED: under the sub-page row (0.5.1: OLED_UNDER)
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph + (o ? LU (25) : 0));
	s_Page = pg;
	const lv_font_t *f = o ? TH->f_micro : TH->f_small;
	const int kind = o ? TH_KEY | TH_KEY_SMALL : TH_KEY, dim = o ? TH->dim : TH->win_dim;
	const int x0 = o ? 0 : 24, w = o ? pw : pw - 48, top = o ? 16 : 44, keyh = o ? LU (22) + TH->pdy / 12 : LU (44) + TH->pdy / 8, gap = o ? 3 : 6;
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, 13);
		th_titlebar_set (s_TitleBar, "LOOP", "", 0);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, 8, 6, pw - 16, ph - 14, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, TH->f_mid, TH->win_ink, "LOOP");
	}
	// row 1: the six grab keys, then STOP and CUT
	const int nk = 8, kw = (w - (nk - 1) * gap) / nk;
	for (int k = 0; k < 6; k++)
		s_Len[k] = th_key (pg, x0 + k * (kw + gap), top, kw, keyh, kLen[k], kind, len_cb, (void *) kLen[k]);
	s_Stop = th_key (pg, x0 + 6 * (kw + gap), top, kw, keyh, "STOP", kind | TH_KEY_DANGER, stop_cb, 0);
	s_Cut = th_key (pg, x0 + 7 * (kw + gap), top, kw, keyh, "CUT", kind | TH_KEY_DANGER, stop_cb, (void *) 1);
	// row 2: the eight grabs, and KEEP under them on the right
	const int py = top + keyh + gap * 2, padh = keyh * 3 / 2, pwid = (w - (SLP_GRABS - 1) * gap) / SLP_GRABS;
	for (int n = 0; n < SLP_GRABS; n++)
		s_Pad[n] = th_key (pg, x0 + n * (pwid + gap), py, pwid, padh, "", kind | TH_KEY_PAD, pad_cb, (void *) (intptr_t) n);
	// row 3: BAL and LEV sliders with their names, KEEP
	const int sy = py + padh + gap * 2, sh = o ? LU (14) + TH->pdy / 24 : LU (28) + TH->pdy / 12, lab = o ? 40 : 84, kk = o ? 44 : 90;
	s_BalL = th_label (pg, x0, sy + (sh - lv_font_get_line_height (f)) / 2, f, o ? TH->ink : TH->win_ink, "BAL");
	s_LevL = th_label (pg, x0, sy + sh + gap + (sh - lv_font_get_line_height (f)) / 2, f, o ? TH->ink : TH->win_ink, "LEV");
	lv_obj_set_width (s_BalL, lab), lv_obj_set_width (s_LevL, lab);
	const int sw = w - lab - kk - 2 * gap;
	s_Bal = th_slider (pg, x0 + lab, sy, sw, sh, 0, 127, level_cb, (void *) (intptr_t) 0);
	s_Lev = th_slider (pg, x0 + lab, sy + sh + gap, sw, sh, 0, 127, level_cb, (void *) (intptr_t) 1);
	s_Keep = th_key (pg, x0 + w - kk, sy, kk, sh * 2 + gap, "KEEP", kind | TH_KEY_ACCENT, keep_cb, 0);
	// row 4: FADE, MUTE, LIVE
	const int ry = sy + 2 * sh + gap * 3, kw3 = (w - 2 * gap) / 3;
	s_Fade = th_key (pg, x0, ry, kw3, keyh, "FADE", kind, fade_cb, 0);
	s_Mute = th_key (pg, x0 + kw3 + gap, ry, kw3, keyh, "MUTE", kind, mute_cb, 0);
	s_Live = th_key (pg, x0 + 2 * (kw3 + gap), ry, kw3, keyh, "LIVE", kind, live_cb, 0);
	// the state line and the foot
	s_State = th_label (pg, x0, ry + keyh + gap, f, o ? TH->ink : TH->win_ink, "");
	lv_obj_set_width (s_State, w);
	s_Info = th_label (pg, x0, ph - (o ? 14 : 36), f, o ? TH->faint : dim, "");
	lv_obj_set_width (s_Info, w);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_loop_set_command (sidecar_loop_command_t fn) { s_Cmd = fn; }

void sidecar_loop_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_loop_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

int sidecar_loop_sel (void) { return s_nSel; }

static void len_text (int hb, char *b, int cap)
{
	if (hb == 1) lv_snprintf (b, cap, "1/2");
	else lv_snprintf (b, cap, "%d", hb / 2);
}

static void refresh (void)
{
	const sidecar_loop_view_t *v = &s_V;
	char t[64], r[32], l[8];
	static const char *const kState[4] = {"RING", "STARTS AT THE NEXT BAR", "PLAYING", "FADING BACK"};
	if (s_nSelId)
	{
		int f = -1;
		for (int i = 0; i < v->ngrabs; i++) if (v->g[i].id == s_nSelId) f = i;
		if (f < 0) s_nSelId = 0;
		s_nSel = f < 0 ? 0 : f;
	}
	else s_nSel = 0;
	if (v->on) lv_snprintf (r, sizeof r, "RING %ds", v->ring_s);
	else lv_snprintf (r, sizeof r, "OFF");
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "LOOP", r, 0);
	else { lv_snprintf (t, sizeof t, "LOOP  %s", r); th_label_set (s_Title, t); }
	for (int k = 0; k < 6; k++) th_key_enable (s_Len[k], v->on);
	th_key_enable (s_Stop, v->on && v->state != 0);
	th_key_enable (s_Cut, v->on && v->state != 0);
	for (int n = 0; n < SLP_GRABS; n++)
	{
		const sidecar_loop_grab_t *g = &v->g[n];
		th_key_enable (s_Pad[n], n < v->ngrabs);
		th_key_check (s_Pad[n], n < v->ngrabs && n == s_nSel);
		th_key_led (s_Pad[n], n == v->playing ? LED_GREEN : n < v->ngrabs && g->kept ? LED_BLUE : 0);
		if (n < v->ngrabs)
		{
			len_text (g->hb, l, sizeof l);
			lv_snprintf (t, sizeof t, "%d", (g->bpm10 + 5) / 10);
			th_key_sub (s_Pad[n], t);
			th_key_font (s_Pad[n], 0);
			lv_snprintf (t, sizeof t, "%s%s", l, g->gap ? "!" : "");
		}
		else th_key_sub (s_Pad[n], ""), t[0] = 0;
		th_key_text (s_Pad[n], t);
	}
	th_key_enable (s_Keep, v->on && v->ngrabs && !v->g[s_nSel].kept && v->g[s_nSel].copied);
	th_slider_set (s_Bal, v->bal);
	th_slider_set (s_Lev, v->lev);
	lv_snprintf (t, sizeof t, "BAL %d", v->bal);
	th_label_set (s_BalL, t);
	lv_snprintf (t, sizeof t, "LEV %d", v->lev);
	th_label_set (s_LevL, t);
	lv_snprintf (t, sizeof t, "FADE %s", kFadeT[v->fade % 5]);
	th_key_text (s_Fade, t);
	lv_snprintf (t, sizeof t, "MUTE %s", v->mute ? "ON" : "OFF");
	th_key_text (s_Mute, t);
	th_key_check (s_Mute, v->mute);
	if (v->live) lv_snprintf (t, sizeof t, "LIVE P%d", v->live);
	else lv_snprintf (t, sizeof t, "LIVE OFF");
	th_key_text (s_Live, t);
	th_key_check (s_Live, v->live != 0);
	if (v->pending) lv_snprintf (t, sizeof t, "%s", v->state == 2 || v->state == 1 ? "RESAMPLE: WAITING FOR THE BAR'S AUDIO" : "GRAB: WAITING FOR THE BAR'S AUDIO");
	else if (v->state != 0 && v->bars10 > 0)
	{
		if (v->bars10 < 10) lv_snprintf (l, sizeof l, "1/2");
		else lv_snprintf (l, sizeof l, "%d", v->bars10 / 10);
		lv_snprintf (t, sizeof t, "%s  %d.%d / %s BARS", kState[v->state & 3], v->pos10 / 10, v->pos10 % 10, l);
	}
	else lv_snprintf (t, sizeof t, "%s", v->on ? kState[v->state & 3] : "");
	th_label_set (s_State, t);
	th_label_set (s_Info, !v->valid ? "WAITING FOR THE ENGINE"
		: !v->on ? "LOOPER OFF: LOOPRAM=0 OR NO RAM (1 GB: LOOPRAM=48)"
		: v->msg[0] ? v->msg
		: "A GRAB TAKES THE LAST BARS; A PAD PLAYS ONE");
}

int sidecar_loop_key (int which, int *x, int *y)
{
	lv_obj_t *o = which < 6 ? s_Len[which] : which == 6 ? s_Stop : which == 7 ? s_Cut : which < 16 ? s_Pad[which - 8] : which == 16 ? s_Keep
		: which == 17 ? s_Fade : which == 18 ? s_Mute : which == 19 ? s_Live : which == 20 ? s_Bal : which == 21 ? s_Lev : 0;
	if (!o) return 0;
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	*x = (a.x1 + a.x2) / 2, *y = (a.y1 + a.y2) / 2;
	return 1;
}

void sidecar_loop_set (const sidecar_loop_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	s_V = *v;
	s_nGen = v->gen;
	refresh ();
}
