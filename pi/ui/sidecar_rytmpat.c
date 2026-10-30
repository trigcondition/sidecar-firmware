// pi/ui/sidecar_rytmpat.c - see sidecar_rytmpat.h. The page is laid out per theme (sidecar_theme.h):
//   CLASSIC (800x480, below the title bar, page coordinates):
//     header  y 4..52     RYTM PATTERN (read only), project / pattern / kit, where the data is from, pages 1-4 / ALL, SEQ
//     ruler   y 56..70    track grid x 52..692 y 70..334 (12 rows of 22 px), names x 8..48
//     locks   y 340..424  the selected track's p-locked parameters, a lane each (value = amber's strength)
//     right   x 700..792  the selected step: note, velocity, length, micro, condition, flags, locks
//   OLED (400x240 dots, left of the navigation column; SEQ there goes back):
//     title   y 0..12     RYTM, the pattern, the kit
//     row     y 15..36    pages 1-4 / ALL, where the data is from
//     ruler   y 39..47    grid x 24..279 y 48..167 (12 rows of 10 dots, 16 dots a step), names x 0..20, the step x 284..351
//     locks   y 171..239  the lock lanes: a grey bar a step, height and brightness the value
// Drawn by code like the editor (sidecar_seqed.c): one object per area, culled to LVGL's redraw area.
#include "sidecar_rytmpat.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"	// lv_area_intersect: culling to the redrawn area
#include <string.h>

typedef struct
{
	int pg_h, gx, gy, gw, rowh, names_x, names_w, rul_y, rul_h, ly, lh, lanes, lane_h, lane_y0, lane_name_x, lane_name_w;
	int card_x, card_y, card_w, card_h, line;
} pat_geo_t;

static const pat_geo_t kGeoClassic = {
	.pg_h = 428, .gx = 52, .gy = 70, .gw = 640, .rowh = 22, .names_x = 8, .names_w = 40, .rul_y = 56, .rul_h = 14,
	.ly = 340, .lh = 84, .lanes = 4, .lane_h = 17, .lane_y0 = 16, .lane_name_x = 698, .lane_name_w = 92,
	.card_x = 700, .card_y = 60, .card_w = 92, .card_h = 274, .line = 26,
};

static const pat_geo_t kGeoOled = {
	.pg_h = 240, .gx = 24, .gy = 48, .gw = 256, .rowh = 10, .names_x = 0, .names_w = 21, .rul_y = 39, .rul_h = 9,
	.ly = 171, .lh = 69, .lanes = 4, .lane_h = 13, .lane_y0 = 13, .lane_name_x = 284, .lane_name_w = 68,
	.card_x = 284, .card_y = 39, .card_w = 68, .card_h = 128, .line = 13,
};

// the table now: the theme's 800x480 one, grown by what a 1024x600 screen adds (TH->dx, dy): the grid wider in whole
// dots a step (OLED 20 instead of 16, CLASSIC 52 instead of 40), the rows taller (13 / 28), up to 6 lock lanes, the
// step card wider
static pat_geo_t s_Geo;
static const pat_geo_t *G = &kGeoClassic;

static void geo_size (const pat_geo_t *b)
{
	pat_geo_t g = *b;
	const int dx = TH->dx, dy = TH->dy, o = TH->id == THEME_OLED;
	const int gx = o ? dx * 4 / 7 : dx * 6 / 7;		// OLED 64 of the 112 dots, CLASSIC 192 of the 224 px
	g.pg_h += dy;
	g.gw += gx;
	g.rowh += dy * 2 / 3 / 12;
	const int gh = 12 * (g.rowh - b->rowh);			// what the rows add
	g.ly += gh;
	g.lh += dy - gh;
	if (dy) g.lanes = (g.lh - g.lane_y0 - (o ? 0 : 4)) / g.lane_h;	// 6 at 1024x600 (4 at 800x480)
	if (g.lanes > 6) g.lanes = 6;
	g.card_x += gx;
	g.card_w += dx - gx;
	g.card_h += gh;
	g.lane_name_x += gx;
	g.lane_name_w += dx - gx;
	s_Geo = g;
	G = &s_Geo;
}
#define GX	(G->gx)
#define GY	(G->gy)
#define GW	(G->gw)
#define ROWH	(G->rowh)
#define GH	(RYTM_TRACKS * ROWH)
#define LY	(G->ly)
#define LH	(G->lh)
#define LANES	(G->lanes)

static int oled (void) { return TH->id == THEME_OLED; }

static lv_obj_t *s_Page, *s_Grid, *s_Names, *s_Ruler, *s_Locks, *s_Head, *s_Info, *s_Where, *s_Card[8], *s_CardT, *s_TitleBar;
static lv_obj_t *s_PageM;
static rytm_pattern_t s_P;
static int s_Index = -1, s_Pg, s_Track, s_Step = -1, s_Play = -1, s_Stale;
static char s_Project[32], s_Kit[32], s_From[64];
static void (*s_Back) (void);

static int nsteps (void) { return s_Pg == 4 ? 64 : 16; }
static int first (void) { return s_Pg == 4 ? 0 : s_Pg * 16; }
static int cellw (void) { return GW / nsteps (); }

#define fill th_fill
#define frame th_frame
#define text th_text

static uint32_t vel_rgb (int vel)
{
	if (vel > 127) vel = 100;
	if (oled ()) return th_oled_secondary (vel);		// the panel's blue print, brighter the louder
	return th_mix (0x16502A, TH->accent_hi, vel * 256 / 127);
}

static uint32_t lock_rgb (int v)
{
	if (oled ()) return th_mix (0x3A3A3A, 0xE6E6E6, v * 256 / 127);		// a lock: grey, brighter the higher
	return th_mix (0x2A1D08, TH->lock, 40 + v * 215 / 127);
}

static int vel_of (const rytm_track_t *k, int s) { return k->vel[s] == RYTM_DEFAULT ? k->dvel : k->vel[s]; }

static void grid_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	int n = nsteps (), f = first (), w = cellw (), o = oled ();
	for (int t = 0; t < RYTM_TRACKS; t++)
	{
		const rytm_track_t *k = &s_P.t[t];
		int y = t * ROWH;
		if (t == s_Track) fill (l, &c, 0, y, GW - 1, y + ROWH - 1, o ? 0x262626 : 0x1E1E1E, LV_OPA_COVER, 0);	// even, no checker behind the trigs
		for (int i = 0; i < n; i++)
		{
			int s = f + i, x = i * w;
			if (o)
			{
				if (s >= k->steps) { fill (l, &c, x, y + ROWH / 2, x + w - 2, y + ROWH / 2, DOT_DOTTED, LV_OPA_COVER, 0); continue; }
				if (t != s_Track) fill (l, &c, x, y + ROWH - 2, x, y + ROWH - 2, s % 4 ? 0x1E1E1E : 0x484848, LV_OPA_COVER, 0);
			}
			else
			{
				uint32_t bg = (s / 4) % 2 ? 0x151515 : 0x1A1A1A;
				if (t == s_Track) bg = (s / 4) % 2 ? 0x1C1C1C : 0x222222;
				if (s >= k->steps) bg = 0x0C0C0C;
				fill (l, &c, x + 1, y + 1, x + w - 2, y + ROWH - 2, bg, LV_OPA_COVER, 2);
			}
			unsigned tr = k->trig[s];
			int locks = rytm_locks_on (&s_P, t, s);
			int mx = k->micro[s] * w / 24;			// micro-timing: the trig moved within its cell
			int bx1, bx2, by1, by2;
			if (o) { bx1 = x + 1 + mx; bx2 = x + w - (w >= 6 ? 3 : 2) + mx; by1 = y + 1; by2 = y + ROWH - 4; }
			else
			{
				bx1 = x + 3 + mx; bx2 = x + w - 4 + mx;
				if (w < 20) { bx1 = x + 1 + mx; bx2 = x + w - 2 + mx; }
				by1 = y + 4; by2 = y + ROWH - 7;
			}
			if (tr & RYTM_TRIG_ON)
			{
				uint32_t col = vel_rgb (vel_of (k, s));
				fill (l, &c, bx1, by1, bx2, by2, col, LV_OPA_COVER, o ? 0 : 2);
				if (tr & RYTM_TRIG_ACCENT) fill (l, &c, bx1, by1, bx2, by1 + (o ? 0 : 1), o ? 0xFFFFFF : 0xE8E8E8, LV_OPA_COVER, 0);
				int len = k->len[s] == RYTM_DEFAULT ? k->dlen : k->len[s];
				int lw = (len + 1) * w / 32;		// the length bar: about LEN / 32 steps [scale inferred]
				if (lw > 4 * w) lw = 4 * w;
				if (o) fill (l, &c, bx1, y + ROWH - 3, bx1 + lw - 1, y + ROWH - 3, th_mix (col, 0, 110), LV_OPA_COVER, 0);
				else fill (l, &c, bx1, y + ROWH - 5, bx1 + lw - 1, y + ROWH - 4, col, LV_OPA_60, 0);
			}
			else if (tr & RYTM_TRIG_LOCK) frame (l, &c, bx1, by1, bx2, by2, o ? DOT_DOTTED : TH->lock, 1, o ? 0 : 2);
			if (locks && w >= (o ? 4 : 10))
			{
				if (o) fill (l, &c, x + w - 3, y + 1, x + w - 2, y + 2, TH->accent, LV_OPA_COVER, 0);
				else fill (l, &c, x + w - 6, y + 2, x + w - 3, y + 4, TH->lock, LV_OPA_COVER, 1);
			}
			if (k->cond[s] && (tr & (RYTM_TRIG_ON | RYTM_TRIG_LOCK)))	// a condition: a dotted line under the trig
			{
				if (o) fill (l, &c, bx1, y + ROWH - 2, bx2, y + ROWH - 2, DOT_DOTTED, LV_OPA_COVER, 0);
				else for (int d = bx1; d <= bx2; d += 3) fill (l, &c, d, y + ROWH - 3, d, y + ROWH - 2, 0xE8E8E8, LV_OPA_80, 0);
			}
			if (t == s_Track && s == s_Step) frame (l, &c, x, y, x + w - 1, y + ROWH - 1, o ? 0xFFFFFF : 0xE8E8E8, o ? 1 : 2, o ? 0 : 3);
		}
	}
}

static void names_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Names, &c);
	const char *const *nm = rytm_track_names ();
	for (int t = 0; t < RYTM_TRACKS; t++)
	{
		int y = t * ROWH, sel = t == s_Track;
		if (oled ())
		{
			if (sel) fill (l, &c, 0, y, G->names_w - 1, y + ROWH - 1, TH->sel_bg, LV_OPA_COVER, 0);
			text (l, &c, 0, y + (ROWH - 10) / 2, G->names_w, TH->f_micro, sel ? TH->sel_ink : TH->dim, LV_TEXT_ALIGN_CENTER, nm[t]);
			continue;
		}
		if (sel) fill (l, &c, 0, y + 1, 39, y + ROWH - 2, TH->accent, LV_OPA_COVER, 3);
		text (l, &c, 0, y + (ROWH - 16) / 2, 40, &lv_font_montserrat_14, sel ? 0x101010 : TH->win_ink, LV_TEXT_ALIGN_CENTER, nm[t]);
	}
}

static void ruler_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Ruler, &c);
	int n = nsteps (), f = first (), w = cellw (), wide = oled () ? w >= 12 : w >= 20;
	const lv_font_t *ft = oled () ? TH->f_micro : &lv_font_montserrat_12;
	for (int i = 0; i < n; i += (wide ? 1 : 4))
	{
		int s = f + i;
		char b[6];
		if (!wide && s % 16 == 0) lv_snprintf (b, sizeof b, "P%d", s / 16 + 1); else lv_snprintf (b, sizeof b, "%d", s + 1);
		uint32_t ink = oled () ? (s % 4 ? TH->faint : TH->dim) : (s % 4 ? 0x6C6E72 : TH->win_dim);
		text (l, &c, i * w, -1, w * (wide ? 1 : 4), ft, ink, wide ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, b);
	}
}

static void locks_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Locks, &c);
	int n = nsteps (), f = first (), w = cellw (), lane = 0, total = 0, o = oled ();
	const char *const *nm = rytm_track_names ();
	for (int i = 0; i < s_P.nparams; i++) total += s_P.lock[i].track == s_Track;
	char b[80];
	const lv_font_t *ft = o ? TH->f_micro : &lv_font_montserrat_12;
	if (o)
	{
		frame (l, &c, 0, 4, G->lane_name_x + G->lane_name_w - 1, LH - 1, DOT_DOTTED, 1, 0);
		char sh[16];
		lv_snprintf (sh, sizeof sh, " (%d SHOWN)", LANES);
		lv_snprintf (b, sizeof b, "P-LOCKS %s  %d%s  POOL %d/%d", nm[s_Track], total, total > LANES ? sh : "",
			     s_P.nlocks, RYTM_LOCKS);
		int tw = th_text_width (ft, b);
		fill (l, &c, 4, 0, 4 + tw + 5, 8, 0x000000, LV_OPA_COVER, 0);
		text (l, &c, 7, -1, 340 + TH->dx, ft, TH->dim, LV_TEXT_ALIGN_LEFT, b);
	}
	else
	{
		char sh[16];
		lv_snprintf (sh, sizeof sh, " (%d shown)", LANES);
		lv_snprintf (b, sizeof b, "P-LOCKS  %s   %d parameter%s locked%s      pattern lock slots %d/%d", nm[s_Track], total,
			     total == 1 ? "" : "s", total > LANES ? sh : "", s_P.nlocks, RYTM_LOCKS);
		text (l, &c, 10, 4, 600 + TH->dx, ft, TH->ink, LV_TEXT_ALIGN_LEFT, b);
	}
	if (!total)
		text (l, &c, GX - (o ? 0 : 2), o ? 28 : 30, 500, o ? TH->f_micro : &lv_font_montserrat_14, o ? TH->faint : TH->dim,
		      LV_TEXT_ALIGN_LEFT, o ? "NO P-LOCKS ON THIS TRACK" : "no p-locks on this track");
	for (int i = 0; i < s_P.nparams && lane < LANES; i++)
	{
		const rytm_lock_t *k = &s_P.lock[i];
		if (k->track != s_Track) continue;
		int y = G->lane_y0 + lane * G->lane_h + (o ? 0 : 4);
		text (l, &c, G->lane_name_x, y + (o ? 0 : 1), G->lane_name_w, ft, o ? TH->accent : 0x8A5A10, LV_TEXT_ALIGN_LEFT, rytm_lock_name (k->type));
		for (int j = 0; j < n; j++)
		{
			int s = f + j, x = GX - (o ? 0 : 2) + j * w, v = k->val[s];
			int y2 = y + G->lane_h - 2;
			if (v == 0xFF)
			{
				if (o) fill (l, &c, x, y + G->lane_h / 2 - 1, x, y + G->lane_h / 2 - 1, 0x1E1E1E, LV_OPA_COVER, 0);
				else fill (l, &c, x + 1, y + 1, x + w - 2, y + 15, 0x9EA0A4, LV_OPA_COVER, 2);
				continue;
			}
			if (o)						// a bar: the value as height, the amber ramp as brightness
			{
				int bh = 1 + v * (G->lane_h - 3) / 127;
				fill (l, &c, x, y2 - bh + 1, x + (w > 2 ? w - 2 : 0), y2, lock_rgb (v), LV_OPA_COVER, 0);
				continue;
			}
			fill (l, &c, x + 1, y + 1, x + w - 2, y + 15, lock_rgb (v), LV_OPA_COVER, 2);
			if (w >= 30)
			{
				lv_snprintf (b, sizeof b, "%d", v);
				text (l, &c, x, y + 1, w, &lv_font_montserrat_12, v > 70 ? 0x101010 : 0xE8E8E8, LV_TEXT_ALIGN_CENTER, b);
			}
		}
		lane++;
	}
}

static void card_update (void)
{
	char b[48];
	const char *const *nm = rytm_track_names ();
	const rytm_track_t *k = &s_P.t[s_Track];
	int o = oled ();
	if (s_Step < 0)
	{
		lv_snprintf (b, sizeof b, "%s  -", nm[s_Track]);
		th_label_set (s_CardT, o ? nm[s_Track] : b);
		for (int i = 0; i < 8; i++) th_label_set (s_Card[i], "");
		th_label_set (s_Card[0], o ? "TAP A STEP" : "tap a step");
		return;
	}
	int s = s_Step;
	unsigned tr = k->trig[s];
	lv_snprintf (b, sizeof b, o ? "%s %d" : "%s  STEP %d", nm[s_Track], s + 1);
	th_label_set (s_CardT, b);
	static const char *const kNote[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
	int note = k->note[s] == RYTM_DEFAULT ? k->dnote : k->note[s];
	lv_snprintf (b, sizeof b, "NOTE %s%d%s", kNote[note % 12], note / 12 - 1, k->note[s] == RYTM_DEFAULT ? "*" : "");
	th_label_set (s_Card[0], tr & RYTM_TRIG_ON ? b : (tr & RYTM_TRIG_LOCK) ? "LOCK TRIG" : "NO TRIG");
	lv_snprintf (b, sizeof b, o ? "VEL %d%s" : "VEL  %d%s", vel_of (k, s), k->vel[s] == RYTM_DEFAULT ? "*" : "");
	th_label_set (s_Card[1], b);
	lv_snprintf (b, sizeof b, o ? "LEN %d%s" : "LEN  %d%s", k->len[s] == RYTM_DEFAULT ? k->dlen : k->len[s], k->len[s] == RYTM_DEFAULT ? "*" : "");
	th_label_set (s_Card[2], b);
	lv_snprintf (b, sizeof b, o ? "MIC %s%d" : "MICRO %s%d/24", k->micro[s] > 0 ? "+" : "", k->micro[s]);
	th_label_set (s_Card[3], b);
	char cn[12];
	lv_snprintf (b, sizeof b, "COND %s", rytm_cond_name (k->cond[s], cn, sizeof cn));	// (T6: labelled in both themes)
	th_label_set (s_Card[4], b);
	lv_snprintf (b, sizeof b, "%s%s%s", tr & RYTM_TRIG_ACCENT ? "ACC " : "", tr & RYTM_TRIG_SWING ? "SWG " : "",
		     tr & RYTM_TRIG_SLIDE ? "SLD" : "");
	th_label_set (s_Card[5], b[0] ? b : "-");
	lv_snprintf (b, sizeof b, "LOCKS %d", rytm_locks_on (&s_P, s_Track, s));
	th_label_set (s_Card[6], b);
	th_label_set (s_Card[7], "");
	for (int i = 0, n = 0; i < s_P.nparams && n < 1; i++)
		if (s_P.lock[i].track == s_Track && s_P.lock[i].val[s] != 0xFF)
		{
			lv_snprintf (b, sizeof b, "%s %d", rytm_lock_name (s_P.lock[i].type), s_P.lock[i].val[s]);
			th_label_set (s_Card[7], b);
			n++;
		}
}

static void inval (void)
{
	lv_obj_invalidate (s_Grid);
	lv_obj_invalidate (s_Names);
	lv_obj_invalidate (s_Ruler);
	lv_obj_invalidate (s_Locks);
	card_update ();
}

void sidecar_rytmpat_select (int track, int step)
{
	s_Track = track < 0 ? 0 : track >= RYTM_TRACKS ? RYTM_TRACKS - 1 : track;
	s_Step = step < 0 || step >= RYTM_STEPS ? -1 : step;
	inval ();
}

void sidecar_rytmpat_page (int page)
{
	s_Pg = page < 0 ? 0 : page > 4 ? 4 : page;
	th_seg_set (s_PageM, s_Pg);
	inval ();
	sidecar_rytmpat_playhead (s_Play);
}

static void touch_cb (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int t = (p.y - c.y1) / ROWH;
	if (o == s_Names) sidecar_rytmpat_select (t, s_Step);
	else sidecar_rytmpat_select (t, first () + (p.x - c.x1) / cellw ());
}

static void page_cb (lv_obj_t *m, int i, void *ud)
{
	(void) m; (void) ud;
	if (i >= 0) sidecar_rytmpat_page (i);
}

static void back_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	if (s_Back) s_Back ();
}

void sidecar_rytmpat_playhead (int step)
{
	s_Play = step;
	int i = step - first ();
	if (step < 0 || i < 0 || i >= nsteps ()) { lv_obj_add_flag (s_Head, LV_OBJ_FLAG_HIDDEN); return; }
	lv_obj_set_x (s_Head, GX + i * cellw () + cellw () / 2 - (oled () ? 0 : 1));
	lv_obj_remove_flag (s_Head, LV_OBJ_FLAG_HIDDEN);
}

static void header (void)
{
	char b[96], pat[8] = "-";
	if (s_Index >= 0) lv_snprintf (pat, sizeof pat, "%c%02d", 'A' + s_Index / 16, s_Index % 16 + 1);
	if (oled ())
	{
		// UX review T6: READ ONLY alone under the title; a stale view says so in the title bar ("NO ANSWER, 12 S OLD")
		// instead of the kit, so it is never cut off
		char l[32], r[64];
		lv_snprintf (l, sizeof l, "RYTM %s", pat);
		lv_snprintf (r, sizeof r, "%s", s_Stale && s_From[0] ? s_From : s_Kit);
		for (char *q = r; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
		th_titlebar_set (s_TitleBar, l, r, 0);		// the project is on the home page (352 dots fit one of them)
		th_label_set (s_Where, "READ ONLY");
		th_label_color (s_Where, TH->dim);
		return;
	}
	{	// the project's name cut in the middle so the pattern and the kit always show
		char rest[48], nm[40];
		lv_snprintf (rest, sizeof rest, "  %s  %s", pat, s_Kit);
		const lv_font_t *f = lv_obj_get_style_text_font (s_Info, 0);
		th_fit_middle (nm, sizeof nm, f, s_Project[0] ? s_Project : "-", (int) lv_obj_get_style_width (s_Info, 0) - th_text_width (f, rest));
		lv_snprintf (b, sizeof b, "%s%s", nm, rest);
	}
	th_label_set (s_Info, b);
	lv_snprintf (b, sizeof b, "READ ONLY%s%s", s_From[0] ? "  /  " : "", s_From);
	th_label_set (s_Where, b);
	th_label_color (s_Where, s_Stale ? TH->warn : 0x8A5A10);
}

static void copy (char *d, const char *s, int cap)
{
	int i = 0;
	for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
	d[i] = 0;
}

void sidecar_rytmpat_where (const char *where, int stale)
{
	if (!strcmp (s_From, where ? where : "") && s_Stale == !!stale) return;	// called often: a label change only
	copy (s_From, where, sizeof s_From);
	s_Stale = !!stale;
	header ();
}

void sidecar_rytmpat_set (const rytm_pattern_t *p, int index, const char *project, const char *kit, const char *where)
{
	memcpy (&s_P, p, sizeof *p);
	s_Index = index;
	copy (s_Project, project, sizeof s_Project);
	copy (s_Kit, kit, sizeof s_Kit);
	copy (s_From, where, sizeof s_From);
	s_Stale = 0;
	header ();
	inval ();
}

static lv_obj_t *drawn (lv_obj_t *p, int x, int y, int w, int h, lv_event_cb_t draw, int touch)
{
	lv_obj_t *o = th_box (p, x, y, w, h);
	lv_obj_add_event_cb (o, draw, LV_EVENT_DRAW_MAIN, 0);
	if (touch)
	{
		lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb (o, touch_cb, LV_EVENT_PRESSED, 0);
	}
	return o;
}

void sidecar_rytmpat_show (int on)
{
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_rytmpat_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

void sidecar_rytmpat_create (void *parent, int y0, void (*back) (void))
{
	s_Back = back;
	geo_size (oled () ? &kGeoOled : &kGeoClassic);
	int o = oled (), dx = TH->dx;
	const int PW = o ? TH->w - 48 : TH->w;			// the page's width
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, PW, G->pg_h);
	s_Page = pg;
	static const char *const kPages[] = {"1", "2", "3", "4", "ALL", ""};
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, PW, 13);
		s_PageM = th_seg (pg, 0, 15, 195 + dx / 2, 22, kPages, TH_KEY_TRIG, page_cb, 0);
		s_Where = th_label (pg, 201 + dx / 2, 22, TH->f_micro, TH->dim, "READ ONLY");
		lv_obj_set_width (s_Where, PW - 201 - dx / 2);
		lv_label_set_long_mode (s_Where, LV_LABEL_LONG_MODE_CLIP);
		s_Info = 0;
	}
	else
	{
		s_TitleBar = 0;
		lv_obj_t *acc = th_box (pg, 12, 8, 3, 40);
		lv_obj_set_style_bg_color (acc, lv_color_hex (TH->lock), 0);
		lv_obj_set_style_bg_opa (acc, LV_OPA_COVER, 0);
		s_Where = th_label (pg, 22, 7, &lv_font_montserrat_12, 0x8A5A10, "READ ONLY");
		lv_obj_set_style_text_letter_space (s_Where, 1, 0);
		lv_obj_set_size (s_Where, 376 + dx, 16);
		lv_label_set_long_mode (s_Where, LV_LABEL_LONG_MODE_DOTS);
		s_Info = th_label (pg, 22, 25, &lv_font_montserrat_18, TH->ink, "-");
		lv_obj_set_size (s_Info, 376 + dx, 24);
		lv_label_set_long_mode (s_Info, LV_LABEL_LONG_MODE_DOTS);
		s_PageM = th_seg (pg, 404 + dx, 4, 260, 48, kPages, TH_KEY_TRIG, page_cb, 0);
		th_seg_font (s_PageM, &lv_font_montserrat_16);
		lv_obj_t *b = th_key (pg, 672 + dx, 4, 120, 48, "SEQ", TH_KEY_FUNC, back_cb, 0);
		th_key_font (b, &lv_font_montserrat_16);
		th_panel (pg, 2, G->rul_y - 4, GX + GW, GH + 22, "", TH_PANEL_WINDOW);	// the grid's display window
	}
	th_seg_set (s_PageM, s_Pg);

	s_Ruler = drawn (pg, GX, G->rul_y, GW, G->rul_h, ruler_draw, 0);
	s_Names = drawn (pg, G->names_x, GY, G->names_w, GH, names_draw, 1);
	s_Grid = drawn (pg, GX, GY, GW, GH, grid_draw, 1);
	if (!o) th_panel (pg, 2, LY - 2, PW - 10, LH + 2, "", TH_PANEL_GROUP);
	s_Locks = drawn (pg, o ? 0 : 2, LY, o ? PW : PW - 10, LH, locks_draw, 0);	// the lane names right of the lanes
	s_Head = th_box (pg, GX, GY - (o ? 1 : 4), o ? 1 : 2, GH + (o ? 2 : 8));
	lv_obj_set_style_bg_color (s_Head, lv_color_hex (TH->play), 0);
	lv_obj_set_style_bg_opa (s_Head, o ? LV_OPA_COVER : LV_OPA_50, 0);
	lv_obj_add_flag (s_Head, LV_OBJ_FLAG_HIDDEN);

	// the step card
	lv_obj_t *cp = th_panel (pg, G->card_x, G->card_y, G->card_w, G->card_h, "", o ? TH_PANEL_BARE : TH_PANEL_WINDOW);
	const lv_font_t *ft = o ? TH->f_micro : &lv_font_montserrat_12;
	s_CardT = th_label (cp, o ? 0 : 8, o ? 0 : 8, o ? TH->f_body : ft, o ? TH->ink : TH->accent, "");
	lv_obj_set_width (s_CardT, G->card_w - (o ? 0 : 12));
	lv_label_set_long_mode (s_CardT, o ? LV_LABEL_LONG_MODE_CLIP : LV_LABEL_LONG_MODE_WRAP);
	for (int i = 0; i < 8; i++)
	{
		s_Card[i] = th_label (cp, o ? 0 : 8, o ? 15 + i * G->line : 44 + i * G->line, ft, i == 7 ? (o ? TH->accent : TH->lock) : o ? TH->ink : TH->win_ink, "");
		lv_obj_set_size (s_Card[i], G->card_w - (o ? 0 : 12), o ? 9 : 16);
		lv_label_set_long_mode (s_Card[i], LV_LABEL_LONG_MODE_CLIP);
	}
	if (!o) th_label (cp, 8, G->card_h - 22, ft, TH->win_dim, "* = track");
	header ();
	card_update ();
	sidecar_rytmpat_playhead (s_Play);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}
