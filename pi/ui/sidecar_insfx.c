// pi/ui/sidecar_insfx.c - see sidecar_insfx.h. Eight dials (th_meter TH_METER_KNOB: slot A's four, slot B's four, the
// TYPE's labels and values as captions), a voice selector, a TYPE key a slot (a tap: the next TYPE), the three sends as
// bars, the buses' chains as text, the voice's OUT route as a row (docs/STEREO.md: a key - a tap: TRK -> PRE -> POST -,
// LEV / PAN / WID as bars with their values, the pair's L / R meters while PRE / POST; a row, not a third column of
// dials: the page has no room for one); every mark through the theme so the OLED theme keeps its dots.
#include "sidecar_insfx.h"
#include "sidecar_theme.h"
#include "sidecar_vlabel.h"
#include "lvgl.h"
#include <string.h>

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Voice, *s_Type[2], *s_Knob[8], *s_Val[8], *s_Send[3], *s_SendL[3],
	*s_Bus[3], *s_Info, *s_Out, *s_OutL[3], *s_OutM[3], *s_Pk[2];
static sidecar_insfx_view_t s_V;
static unsigned s_nGen = ~0u;
static int s_nVoice = 0, s_bPicked = 0, s_nShown = -1;
static sidecar_insfx_command_t s_Cmd;
static const char *const kVoiceMap[] = {"BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", ""};
static const char *const kTypeName[10] = {"OFF", "FILTER", "CRUSH", "DRIVE", "EQ", "COMP", "DELAY", "TREMOLO", "RINGMOD", "AMP"};

static int oled (void) { return TH->id == THEME_OLED; }

static void refresh (int all);

static void voice_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) ud;
	s_nVoice = kVoiceOrder[i & 7], s_bPicked = 1;
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
	int t = (s_V.slot[s_nVoice][s].type + 1) % 10;
	char c[40];
	lv_snprintf (c, sizeof c, "ins=V%d:%c:type:%s", s_nVoice + 1, 'A' + s, kTypeName[t]);
	if (s_Cmd) s_Cmd (c);
}

void sidecar_insfx_create (void *parent, int y0)
{
	int o = oled ();
	s_nGen = ~0u, s_nShown = -1;
	memset (&s_V, 0, sizeof s_V);
	// 1024x600 (TH->dx, dy): the same rows, wider, the keys and knobs a little taller (e)
	const int dx = TH->dx, dy = TH->dy, e = dy / 6;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, o ? TH->w - 48 : TH->w, o ? TH->h : 428 + dy);
	s_Page = pg;
	if (o)
	{
		const int sp = 177 + dx / 2, kp = 44 + dx / 8;
		s_TitleBar = th_titlebar (pg, 0, 0, 352 + dx, 13);
		th_titlebar_set (s_TitleBar, "INSERT FX", "", 0);
		s_Title = 0;
		s_Voice = th_seg (pg, 0, 16, 352 + dx, 21 + e, kVoiceMap, TH_KEY | TH_KEY_SMALL, voice_cb, 0);
		for (int s = 0; s < 2; s++)
		{
			s_Type[s] = th_key (pg, s * sp, 41 + e, sp - 2, 21 + e, "A OFF", TH_KEY | TH_KEY_SMALL, type_cb, (void *) (intptr_t) s);
			for (int k = 0; k < 4; k++)
			{
				int i = s * 4 + k, x = s * sp + k * kp;
				s_Knob[i] = th_meter (pg, x, 66 + 2 * e, kp - 2, 51 + e, 127, TH_METER_KNOB, s ? TH->ink : th_oled_secondary (127));
				s_Val[i] = th_label (pg, x, 118 + 3 * e, TH->f_micro, TH->dim, "");
				lv_obj_set_width (s_Val[i], kp - 1);
				lv_obj_set_style_text_align (s_Val[i], LV_TEXT_ALIGN_CENTER, 0);
			}
		}
		for (int b = 0; b < 3; b++)
		{
			const int y = 136 + 3 * e + b * (16 + e / 2);
			s_SendL[b] = th_label (pg, 0, y, TH->f_micro, TH->dim, "");
			lv_obj_set_width (s_SendL[b], 72);
			s_Send[b] = th_meter (pg, 74, y + 1, 76 + dx / 2, 7, 127, TH_METER_H, TH->ink);
			s_Bus[b] = th_label (pg, 158 + dx / 2, y, TH->f_micro, TH->dim, "");
			lv_obj_set_width (s_Bus[b], 194 + dx / 2);
		}
		{	// the OUT row
			const int y = 136 + 3 * e + 3 * (16 + e / 2), g = 76 + dx / 6;
			s_Out = th_key (pg, 0, y - 2, 70, 13 + e / 2, "OUT TRK", TH_KEY | TH_KEY_SMALL, out_cb, 0);
			for (int k = 0; k < 3; k++)
			{
				s_OutL[k] = th_label (pg, 74 + k * g, y, TH->f_micro, TH->dim, "");
				lv_obj_set_width (s_OutL[k], 40);
				s_OutM[k] = th_meter (pg, 116 + k * g, y + 1, g - 46, 7, 127, TH_METER_H, TH->ink);
			}
			for (int c = 0; c < 2; c++)
				s_Pk[c] = th_meter (pg, 74 + 3 * g, y + c * 5, 352 + dx - (74 + 3 * g), 3, 127, TH_METER_H, th_oled_secondary (127));
		}
		s_Info = th_label (pg, 0, 206 + dy, TH->f_micro, TH->faint, "");
		lv_obj_set_width (s_Info, 352 + dx);
	}
	else
	{
		const int sp = 380 + dx / 2, kp = 94 + dx / 8;
		s_TitleBar = 0;
		th_panel (pg, 8, 6, 784 + dx, 414 + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "INSERT FX");
		s_Voice = th_seg (pg, 24, 44, 752 + dx, 40 + e / 2, kVoiceMap, TH_KEY, voice_cb, 0);
		for (int s = 0; s < 2; s++)
		{
			s_Type[s] = th_key (pg, 24 + s * sp, 96 + e, sp - 8, 40 + e / 2, "A OFF", TH_KEY, type_cb, (void *) (intptr_t) s);
			for (int k = 0; k < 4; k++)
			{
				int i = s * 4 + k, x = 24 + s * sp + k * kp;
				s_Knob[i] = th_meter (pg, x, 146 + e + e / 2, kp - 8, 110 + e, 127, TH_METER_KNOB, TH->win_ink);
				s_Val[i] = th_label (pg, x, 258 + 2 * e + e / 2, &lv_font_montserrat_14, TH->win_dim, "");
				lv_obj_set_width (s_Val[i], kp - 8);
				lv_obj_set_style_text_align (s_Val[i], LV_TEXT_ALIGN_CENTER, 0);
			}
		}
		for (int b = 0; b < 3; b++)
		{
			const int y = 286 + 3 * e + b * (28 + e / 2);
			s_SendL[b] = th_label (pg, 24, y, &lv_font_montserrat_16, TH->win_ink, "");
			lv_obj_set_width (s_SendL[b], 200);
			s_Send[b] = th_meter (pg, 230, y + 4, 200 + dx / 2, 14, 127, TH_METER_H, TH->win_ink);
			s_Bus[b] = th_label (pg, 450 + dx / 2, y, &lv_font_montserrat_16, TH->win_dim, "");
			lv_obj_set_width (s_Bus[b], 326 + dx / 2);
		}
		{	// the OUT row
			const int y = 286 + 3 * e + 3 * (28 + e / 2), g = 160 + dx / 6;
			s_Out = th_key (pg, 24, y - 2, 140, 26 + e / 4, "OUT TRK", TH_KEY, out_cb, 0);
			for (int k = 0; k < 3; k++)
			{
				s_OutL[k] = th_label (pg, 176 + k * g, y + 2, &lv_font_montserrat_14, TH->win_ink, "");
				lv_obj_set_width (s_OutL[k], 74);
				s_OutM[k] = th_meter (pg, 252 + k * g, y + 6, g - 84, 10, 127, TH_METER_H, TH->win_ink);
			}
			for (int c = 0; c < 2; c++)
				s_Pk[c] = th_meter (pg, 176 + 3 * g, y + 2 + c * 12, 776 + dx - (176 + 3 * g), 8, 127, TH_METER_H, TH->win_ink);
		}
		s_Info = th_label (pg, 24, 402 + dy, &lv_font_montserrat_14, TH->win_dim, "");
		lv_obj_set_width (s_Info, 752 + dx);
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

static void refresh (int all)
{
	const sidecar_insfx_view_t *v = &s_V;
	int vi = s_nVoice, o = oled ();
	char t[96], r[24];
	if (all || s_nShown != vi)
	{
		s_nShown = vi;
		th_seg_set (s_Voice, kVoicePos[vi & 7]);
	}
	lv_snprintf (r, sizeof r, "%s  KNOBS ON RYTM", kVoiceName[vi & 7]);	// T10: the knobs mirror the Rytm's (read only here)
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "INSERT FX", r, 0);
	else { lv_snprintf (t, sizeof t, "INSERT FX  %s", r); th_label_set (s_Title, t); }
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
			th_meter_label (s_Knob[i], x->type ? x->label[k] : "");
			th_label_set (s_Val[i], x->type ? x->val[k] : "");
		}
	}
	for (int b = 0; b < 3; b++)
	{
		lv_snprintf (t, sizeof t, o ? "SEND B%d %d" : "Send to bus %d: %d", b + 1, v->send[vi][b]);
		th_label_set (s_SendL[b], t);
		th_meter_set (s_Send[b], v->send[vi][b]);
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
					   : "FOLLOWS THE RYTM'S TRACK. EDIT: THE RYTM'S INSERT PAGE");
}

void sidecar_insfx_set (const sidecar_insfx_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	int follow = v->voice >= 0 && (v->voice != s_V.voice || v->track != s_V.track);
	s_V = *v;
	s_nGen = v->gen;
	if (follow) s_nVoice = v->voice, s_bPicked = 0;	// the Rytm selected another track: its voice
	refresh (0);
}
