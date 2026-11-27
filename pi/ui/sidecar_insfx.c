// pi/ui/sidecar_insfx.c - see sidecar_insfx.h. Eight dials (th_meter TH_METER_KNOB: slot A's four, slot B's four, the
// TYPE's labels and values as captions), a voice selector, a TYPE key a slot (a tap: the next TYPE), the three sends as
// bars, the buses' chains as text, the voice's OUT route as a row (docs/STEREO.md: a key - a tap: TRK -> PRE -> POST -,
// LEV / PAN / WID as bars with their values, the pair's L / R meters while PRE / POST; a row, not a third column of
// dials: the page has no room for one); every mark through the theme so the OLED theme keeps its dots.
#include "sidecar_insfx.h"
#include "sidecar_theme.h"
#include "sidecar_vlabel.h"
#include "sidecar_ui.h"		// sidecar_ui_voice: the box's names
#include "lvgl.h"
#include <string.h>

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Voice, *s_Type[2], *s_Knob[8], *s_Val[8], *s_Send[3], *s_SendL[3],
	*s_Bus[3], *s_Info, *s_Out, *s_OutL[3], *s_OutM[3], *s_Pk[2];
static sidecar_insfx_view_t s_V;
static unsigned s_nGen = ~0u;
static int s_nVoice = 0, s_bPicked = 0, s_nShown = -1;
static sidecar_insfx_command_t s_Cmd;
static const char *s_VoiceMap[9];	// the voices in list order, by the box's names (voice_map)
static const char *const *voice_map (void)
{
	for (int i = 0; i < 8; i++) s_VoiceMap[i] = sidecar_ui_voice (sidecar_ui_voice_at (i));
	s_VoiceMap[8] = "";
	return s_VoiceMap;
}
// a voice slot's TYPE key: the INSERT TYPEs only (the send FX TYPEs, 11..16, are the buses' and the master's: they run on
// core 2, a voice's chain on its own render core; the bus chains here are text, named by the view)
static const char *const kTypeName[11] = {"OFF", "FILTER", "CRUSH", "DRIVE", "EQ", "COMP", "DELAY", "TREMOLO", "RINGMOD", "AMP", "SAT"};

static int oled (void) { return TH->id == THEME_OLED; }

static void refresh (int all);

// the followed voice changes: a drag or a held value of the old one must not go on to the new (its lines carry the voice shown)
static void cancel_all (void)
{
	for (int i = 0; i < 8; i++) if (s_Knob[i]) th_meter_edit_cancel (s_Knob[i]);
	for (int i = 0; i < 3; i++) if (s_Send[i]) th_meter_edit_cancel (s_Send[i]);
	for (int i = 0; i < 3; i++) if (s_OutM[i]) th_meter_edit_cancel (s_OutM[i]);
}

static void voice_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) ud;
	cancel_all ();
	s_nVoice = sidecar_ui_voice_at (i), s_bPicked = 1;
	refresh (1);
}

static void out_cb (lv_obj_t *o, int i, void *ud)	// the OUT key: the next route
{
	(void) o; (void) i; (void) ud;
	static const char *const kR[3] = {"pre", "post", "trk"};
	char c[24];
	lv_snprintf (c, sizeof c, "out=V%d:%s", s_nVoice + 1, kR[s_V.out_route[s_nVoice] % 3]);
	if (s_Cmd) s_Cmd (c);
}

static void type_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int s = (int) (intptr_t) ud;
	int t = (s_V.slot[s_nVoice][s].type + 1) % 11;
	char c[40];
	lv_snprintf (c, sizeof c, "ins=V%d:%c:type:%s", s_nVoice + 1, 'A' + s, kTypeName[t]);
	if (s_Cmd) s_Cmd (c);
}

// the touch edits (th_meter_edit: at most every 30 ms while dragging, the final value, the double tap's default): the console's
// lines an encoder turn sends. The send's text follows the finger at once (the others' texts are the engine's, they follow it)
static void knob_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	int i = (int) (intptr_t) ud;
	char c[40];
	lv_snprintf (c, sizeof c, "ins=V%d:%c:p%d:%d", s_nVoice + 1, 'A' + i / 4, i % 4 + 1, v & 0xFFFF);
	if (s_Cmd) s_Cmd (c);
}
static void send_text (int b, int v)
{
	char t[32];
	lv_snprintf (t, sizeof t, oled () ? "SEND B%d %d" : "Send to bus %d: %d", b + 1, v);
	th_label_set (s_SendL[b], t);
}
static void send_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	int b = (int) (intptr_t) ud;
	char c[40];
	lv_snprintf (c, sizeof c, "ins=V%d:send:%d:%d", s_nVoice + 1, b + 1, v & 0xFFFF);
	send_text (b, v & 0xFFFF);
	if (s_Cmd) s_Cmd (c);
}
static void outp_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	static const char *const kK[3] = {"lev", "pan", "wid"};
	char c[40];
	lv_snprintf (c, sizeof c, "out=V%d:%s:%d", s_nVoice + 1, kK[(int) (intptr_t) ud], v & 0xFFFF);
	if (s_Cmd) s_Cmd (c);
}

void sidecar_insfx_create (void *parent, int y0)
{
	int o = oled ();
	s_nGen = ~0u, s_nShown = -1;
	memset (&s_V, 0, sizeof s_V);
	// 1024x600 (TH->dx, dy): the same rows, wider, the keys and knobs a little taller (e). 0.5.1: below the sub-page row the page has
	// 58 px (25 dots) less than it had; the keys stay at the touch size (44 px, OLED 21 dots) and the title shares the voice
	// keys' row, so the rows below are a little tighter (the knobs shorter) instead
	const int dx = TH->dx, dy = TH->pdy, e = TH->dy / 6;
	const int K = (o ? LU (22) : LU (44)) + e / 2;			// a key
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, th_page_w (), o ? TH->h : LU (428) + dy);
	s_Page = pg;
	if (o)
	{
		const int sp = LU (177) + dx / 2, kp = LU (44) + dx / 8, g = LU (3);
		const int yv = LU (16), yt = yv + K + g, yk = yt + K + g, hk = LU (46) + e, yval = yk + hk + 1, ys = yval + LU (9), pit = LU (14) + e / 2;
		const int yo = ys + 3 * pit + LU (2);
		s_TitleBar = th_titlebar (pg, 0, 0, LU (352) + dx, LU (13));
		th_titlebar_set (s_TitleBar, "INSERT FX", "", 0);
		s_Title = 0;
		s_Voice = th_seg (pg, 0, yv, LU (352) + dx, K, voice_map (), TH_KEY | TH_KEY_SMALL, voice_cb, 0);
		for (int s = 0; s < 2; s++)
		{
			s_Type[s] = th_key (pg, s * sp, yt, sp - 2, K, "A OFF", TH_KEY | TH_KEY_SMALL, type_cb, (void *) (intptr_t) s);
			for (int k = 0; k < 4; k++)
			{
				int i = s * 4 + k, x = s * sp + k * kp;
				s_Knob[i] = th_meter (pg, x, yk, kp - 2, hk, 127, TH_METER_KNOB, s ? TH->ink : th_oled_secondary (127));
				th_meter_edit (s_Knob[i], knob_cb, (void *) (intptr_t) i);
				s_Val[i] = th_label (pg, x, yval, TH->f_micro, TH->dim, "");
				lv_obj_set_width (s_Val[i], kp - 1);
				lv_obj_set_style_text_align (s_Val[i], LV_TEXT_ALIGN_CENTER, 0);
			}
		}
		for (int b = 0; b < 3; b++)
		{
			const int y = ys + b * pit;
			s_SendL[b] = th_label (pg, 0, y, TH->f_micro, TH->dim, "");
			lv_obj_set_width (s_SendL[b], LU (72));
			s_Send[b] = th_meter (pg, 0, y, LU (150) + dx / 2, pit, 127, TH_METER_H, TH->ink);	// (the row is the touch target)
			th_meter_inset (s_Send[b], LU (74), 1, 0, pit - 1 - LU (7));
			th_meter_edit (s_Send[b], send_cb, (void *) (intptr_t) b);
			s_Bus[b] = th_label (pg, LU (158) + dx / 2, y, TH->f_micro, TH->dim, "");
			lv_obj_set_width (s_Bus[b], LU (194) + dx / 2);
		}
		{	// the OUT row
			const int y = yo + (K - LU (7)) / 2, g2 = LU (76) + dx / 6;
			s_Out = th_key (pg, 0, yo, LU (70), K, "OUT TRK", TH_KEY | TH_KEY_SMALL, out_cb, 0);
			for (int k = 0; k < 3; k++)
			{
				s_OutL[k] = th_label (pg, LU (74) + k * g2, y, TH->f_micro, TH->dim, "");
				lv_obj_set_width (s_OutL[k], LU (40));
				s_OutM[k] = th_meter (pg, LU (74) + k * g2, yo, g2 - LU (4), K, 127, TH_METER_H, TH->ink);	// (label and bar: the touch target)
				th_meter_inset (s_OutM[k], LU (42), y - yo + 1, 0, K - (y - yo + 1) - LU (7));
				th_meter_edit (s_OutM[k], outp_cb, (void *) (intptr_t) k);
			}
			for (int c = 0; c < 2; c++)
				s_Pk[c] = th_meter (pg, LU (74) + 3 * g2, y + c * 5, LU (352) + dx - (LU (74) + 3 * g2), 3, 127, TH_METER_H, th_oled_secondary (127));
		}
		s_Info = th_label (pg, 0, yo + K + LU (3), TH->f_micro, TH->faint, "");
		lv_obj_set_width (s_Info, LU (352) + dx);
	}
	else
	{
		const int sp = LU (380) + dx / 2, kp = LU (94) + dx / 8, g = LU (8);
		const int yv = LU (12), yt = yv + K + g, yk = yt + K + g, hk = LU (84) + e, yval = yk + hk + 2 + (TH->f_pxs ? LU (6) : 0), ys = yval + LU (18) + (TH->f_pxs ? LU (10) : 0), pit = LU (22) + e / 2;
		const int yo = ys + 3 * pit + LU (2);
		s_TitleBar = 0;
		th_panel (pg, LU (8), LU (6), LU (784) + dx, LU (414) + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, LU (24), yv + (K - TH->f_mid->line_height) / 2, TH->f_mid, TH->win_ink, "INSERT FX");
		lv_obj_set_width (s_Title, LU (276));
		lv_label_set_long_mode (s_Title, LV_LABEL_LONG_MODE_CLIP);
		s_Voice = th_seg (pg, LU (304), yv, LU (472) + dx, K, voice_map (), TH_KEY, voice_cb, 0);
		for (int s = 0; s < 2; s++)
		{
			s_Type[s] = th_key (pg, LU (24) + s * sp, yt, sp - LU (8), K, "A OFF", TH_KEY, type_cb, (void *) (intptr_t) s);
			for (int k = 0; k < 4; k++)
			{
				int i = s * 4 + k, x = 24 + s * sp + k * kp;
				s_Knob[i] = th_meter (pg, x, yk, kp - LU (8), hk, 127, TH_METER_KNOB, TH->accent);
				th_meter_edit (s_Knob[i], knob_cb, (void *) (intptr_t) i);
				s_Val[i] = th_label (pg, x, yval, TH->f_pxs ? TH->f_pxs : TH->f_small, TH->f_pxs ? TH->win_ink : TH->win_dim, "");	// the value in the pixel face
				lv_obj_set_width (s_Val[i], kp - LU (8));
				lv_obj_set_style_text_align (s_Val[i], LV_TEXT_ALIGN_CENTER, 0);
			}
		}
		for (int b = 0; b < 3; b++)
		{
			const int y = ys + b * pit;
			s_SendL[b] = th_label (pg, LU (24), y, TH->f_body, TH->win_ink, "");
			lv_obj_set_width (s_SendL[b], LU (200));
			s_Send[b] = th_meter (pg, LU (24), y, LU (406) + dx / 2, pit, 127, TH_METER_H, TH->win_ink);	// (the row is the touch target)
			th_meter_inset (s_Send[b], LU (206), LU (4), 0, pit - LU (4) - LU (14));
			th_meter_edit (s_Send[b], send_cb, (void *) (intptr_t) b);
			s_Bus[b] = th_label (pg, LU (450) + dx / 2, y, TH->f_body, TH->win_dim, "");
			lv_obj_set_width (s_Bus[b], LU (326) + dx / 2);
		}
		{	// the OUT row
			const int y = yo + (K - LU (16)) / 2, g2 = LU (160) + dx / 6;
			s_Out = th_key (pg, LU (24), yo, LU (140), K, "OUT TRK", TH_KEY, out_cb, 0);
			for (int k = 0; k < 3; k++)
			{
				s_OutL[k] = th_label (pg, LU (176) + k * g2, y, TH->f_small, TH->win_ink, "");
				lv_obj_set_width (s_OutL[k], LU (74));
				s_OutM[k] = th_meter (pg, LU (176) + k * g2, yo, g2 - LU (8), K, 127, TH_METER_H, TH->win_ink);	// (label and bar: the touch target)
				th_meter_inset (s_OutM[k], LU (76), y - yo + LU (4), 0, K - (y - yo + LU (4)) - LU (10));
				th_meter_edit (s_OutM[k], outp_cb, (void *) (intptr_t) k);
			}
			for (int c = 0; c < 2; c++)
				s_Pk[c] = th_meter (pg, LU (176) + 3 * g2, y + c * LU (12), LU (776) + dx - (LU (176) + 3 * g2), LU (8), 127, TH_METER_H, TH->win_ink);
		}
		s_Info = th_label (pg, LU (24), yo + K + LU (4), TH->f_small, TH->win_dim, "");
		lv_obj_set_width (s_Info, LU (752) + dx);
	}
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_insfx_set_command (sidecar_insfx_command_t fn) { s_Cmd = fn; }

void sidecar_insfx_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_insfx_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
int sidecar_insfx_voice (void) { return s_nVoice; }
int sidecar_insfx_hit (int what, int *x, int *y)	// the middle of a key: voice key 0 (0), slot A's TYPE (1), OUT (2); UI coordinates
{
	lv_obj_t *o = what == 0 ? (s_Voice ? th_seg_item (s_Voice, 0) : 0) : what == 1 ? s_Type[0] : what == 2 ? s_Out : 0;
	if (!o) return 0;
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	*x = (c.x1 + c.x2) / 2, *y = (c.y1 + c.y2) / 2;
	return 1;
}

void *sidecar_insfx_obj (int what)
{
	return what >= 0 && what < 8 ? s_Knob[what] : what >= 10 && what < 13 ? s_Send[what - 10] : what >= 20 && what < 23 ? s_OutM[what - 20] : 0;
}

static void refresh (int all)
{
	const sidecar_insfx_view_t *v = &s_V;
	int vi = s_nVoice, o = oled ();
	char t[96], r[24];
	if (all || s_nShown != vi)
	{
		s_nShown = vi;
		th_seg_set (s_Voice, sidecar_ui_voice_pos (vi));
	}
	lv_snprintf (r, sizeof r, "%s  KNOBS ON %s", sidecar_ui_voice (vi), sidecar_ui_box_up ());	// T10: the knobs mirror the Rytm's (read only here)
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "INSERT FX", r, 0);
	else
	{
		lv_snprintf (t, sizeof t, "INSERT FX  %s", r);
		if (th_text_width (TH->f_mid, t) > (int) lv_obj_get_style_width (s_Title, 0))	// (800x480: the title shares the voice keys' row)
			lv_snprintf (t, sizeof t, "INSERT FX  %s", sidecar_ui_voice (vi));
		th_label_set (s_Title, t);
	}
	for (int s = 0; s < 2; s++)
	{
		const sidecar_insfx_slot_t *x = &v->slot[vi][s];
		lv_snprintf (t, sizeof t, o ? "%c %s" : "%c  %s", 'A' + s, x->type ? x->name : "OFF  (TAP: NEXT TYPE)");
		th_key_text (s_Type[s], t);
		th_key_check (s_Type[s], x->type != 0);
		for (int k = 0; k < 4; k++)
		{
			int i = s * 4 + k;
			th_meter_set (s_Knob[i], x->type ? x->v[k] : -1);
			th_meter_edit_set (s_Knob[i], x->base[k], x->def[k], v->valid && x->type);
			th_meter_label (s_Knob[i], x->type ? x->label[k] : "");
			th_label_set (s_Val[i], x->type ? x->val[k] : "");
		}
	}
	for (int b = 0; b < 3; b++)
	{
		th_meter_set (s_Send[b], v->send[vi][b]);
		th_meter_edit_set (s_Send[b], v->send[vi][b], 0, v->valid);
		send_text (b, th_meter_editing (s_Send[b]) ? th_meter_get (s_Send[b]) : v->send[vi][b]);
		const sidecar_insfx_slot_t *a = &v->slot[8 + b][0], *c = &v->slot[8 + b][1];
		lv_snprintf (t, sizeof t, "B%d %s > %s > %s%s %s", b + 1, b < 2 ? v->bus_fx[b] : "DRY", a->type ? a->name : "-",
			     c->type ? c->name : "-", v->peq[8 + b][0] ? " > POST EQ" : "", v->bus_post[b] ? "POST" : "PRE");
		th_label_set (s_Bus[b], t);
	}
	{	// the OUT row
		static const char *const kR[3] = {"TRK", "PRE", "POST"}, *const kL[3] = {"LEV", "PAN", "WID"};
		int r = v->out_route[vi] % 3;
		lv_snprintf (t, sizeof t, "OUT %s", kR[r]);
		th_key_text (s_Out, t);
		th_key_check (s_Out, r != 0);
		for (int k = 0; k < 3; k++)
		{
			lv_snprintf (t, sizeof t, "%s %s", kL[k], v->out_val[vi][k]);
			th_label_set (s_OutL[k], t);
			th_meter_set (s_OutM[k], r ? v->out_v[vi][k] : -1);
			th_meter_edit_set (s_OutM[k], v->out_base[vi][k], v->out_def[k], v->valid && r);	// (TRK: no stage to set)
		}
		for (int c = 0; c < 2; c++) th_meter_set (s_Pk[c], r ? v->out_peak[vi][c] : -1);
	}
	// ins1112: the voice's POST EQ (after A and B) takes the hint line while it is doing something; the Rytm's
	// INSERT page edits it (RIGHT: the EQ view)
	if (v->valid && v->peq[vi][0])
	{
		lv_snprintf (t, sizeof t, "POST EQ (AFTER A, B): %s", v->peq[vi]);
		th_label_set (s_Info, t);
	}
	else
		th_label_set (s_Info, !v->valid ? "WAITING FOR THE ENGINE" : s_bPicked ? "A VOICE KEY: THAT VOICE; THE RYTM'S TRACK SELECT: BACK"
			: v->out_route[vi] ? "PRE / POST: STEREO, THE RYTM'S FLTR AMP DEL REV LEFT OUT. EDIT: INSERT PAGE, YES TWICE"
					   : "FOLLOWS THE RYTM'S TRACK. DRAG A DIAL OR BAR; DOUBLE TAP: DEFAULT");
}

void sidecar_insfx_set (const sidecar_insfx_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	int follow = v->voice >= 0 && (v->voice != s_V.voice || v->track != s_V.track);
	s_V = *v;
	s_nGen = v->gen;
	if (follow) cancel_all (), s_nVoice = v->voice, s_bPicked = 0;	// the Rytm selected another track: its voice
	refresh (0);
}
