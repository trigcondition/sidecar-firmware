// pi/ui/sidecar_song.c - see sidecar_song.h. One code-drawn timeline (DRAW_MAIN: every mark through th_fill / th_text,
// so the OLED theme keeps its dots), an info line, a row of keys, and an overlay for the replay's confirmation and the
// MIDI CONFIG how-to.
#include "sidecar_song.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Info, *s_Msg, *s_Area, *s_Key[8], *s_Undo, *s_Dlg, *s_DlgText[10], *s_DlgYes, *s_DlgNo;
static sidecar_song_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_song_cmd_t s_Cmd;
static int s_nZoom;			// index into kZoom
static unsigned s_nStart;		// the first tick shown (zoomed)
static int s_bFollow = 1, s_bExplain;
static int s_nDragX0;
static unsigned s_nDragStart0;
static const int kZoom[] = {0, 64, 32, 16, 8, 4, 2, 1};	// bars across (0: the whole take)
enum { K_ARM, K_REPLAY, K_PREV, K_NEXT, K_OUT, K_IN, K_FOLLOW, K_CFG, K_UNDO };
static const char *const kTrk[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
static const char *const kHowTo[] = {
	"SONG RECORD HEARS THE RYTM'S MIDI OUT.",
	"SET ON THE RYTM (THE SIDECAR NEVER DOES):",
	"GLOBAL > MIDI CONFIG > PORT CONFIG:",
	"  ENCODER DEST = INT+EXT  (KNOBS SEND)",
	"  OUTPUT CH = TRK CH  (WHICH TRACK)",
	"  PARAM OUTPUT = NRPN  (FINE STEPS)",
	"GLOBAL > MIDI CONFIG > SYNC (REPLAY):",
	"  PROG CH RECEIVE = ON  (PATTERNS)",
	"  PROG CH SEND, TRANSPORT SEND = ON",
};

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_bg (void) { return oled () ? TH->bg : TH->win; }
static uint32_t c_rytm (void) { return oled () ? th_oled_secondary (110) : 0x5A8FD8; }	// the panel's blue print
static uint32_t c_side (void) { return oled () ? th_oled_accent (110) : TH->accent; }	// the FUNC teal / the accent

static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

// ---- the timeline's geometry
static unsigned span (void)
{
	unsigned len = s_V.length > s_V.playhead ? s_V.length : s_V.playhead;
	if (len < 4 * SSG_BAR) len = 4 * SSG_BAR;
	return kZoom[s_nZoom] ? (unsigned) kZoom[s_nZoom] * SSG_BAR : len + len / 32;
}
static unsigned total (void)
{
	unsigned len = s_V.length > s_V.playhead ? s_V.length : s_V.playhead;
	return len < 4 * SSG_BAR ? 4 * SSG_BAR : len;
}
static int lw (void) { return oled () ? 51 : 80; }	// the lanes' names (8 micro characters: "LT SRC C", T12)
static int xof (unsigned t, int w)
{
	long long d = (long long) t - (long long) s_nStart;
	return lw () + (int) (d * (w - lw ()) / (long long) span ());
}

static void follow (void)
{
	if (!kZoom[s_nZoom]) { s_nStart = 0; return; }
	if (!s_bFollow) return;
	unsigned sp = span (), ph = s_V.playhead;
	if (s_V.state != SSG_REC && s_V.state != SSG_REPLAYING) return;
	if (ph < s_nStart || ph > s_nStart + sp * 4 / 5) s_nStart = ph > sp * 2 / 5 ? ph - sp * 2 / 5 : 0;
	s_nStart -= s_nStart % SSG_BAR;
}

// a lane's row: its name at the left, a faint base line
static void lane_name (lv_layer_t *l, const lv_area_t *c, int y, int h, const char *name, uint32_t rgb)
{
	const lv_font_t *f = TH->f_micro;
	int fh = lv_font_get_line_height (f);
	th_text (l, c, 1, y + (h - fh) / 2, lw () - 2, f, rgb, LV_TEXT_ALIGN_LEFT, name);
}

static void curve (lv_layer_t *l, const lv_area_t *c, int w, int y, int h, const ssg_point_t *p, int n, unsigned vmax,
		   uint32_t rgb, int filled)
{
	if (n <= 0) return;
	for (int i = 0; i < n; i++)
	{
		unsigned t1 = i + 1 < n ? p[i + 1].t : total ();
		int x0 = xof (p[i].t, w), x1 = xof (t1, w);
		if (x1 < lw () || x0 >= w) continue;
		if (x0 < lw ()) x0 = lw ();
		if (x1 > w - 1) x1 = w - 1;
		int vy = y + h - 1 - (int) ((unsigned long) p[i].v * (unsigned) (h - 1) / (vmax ? vmax : 1));
		if (filled) th_fill (l, c, x0, vy, x1 > x0 ? x1 : x0, y + h - 1, th_mix (c_bg (), rgb, 90), LV_OPA_COVER, 0);
		th_fill (l, c, x0, vy, x1 > x0 ? x1 : x0, vy, rgb, LV_OPA_COVER, 0);			// stepped: held until the next
		if (i + 1 < n && x1 >= lw () && x1 < w)
		{
			int ny = y + h - 1 - (int) ((unsigned long) p[i + 1].v * (unsigned) (h - 1) / (vmax ? vmax : 1));
			th_fill (l, c, x1, vy < ny ? vy : ny, x1, vy < ny ? ny : vy, rgb, LV_OPA_COVER, 0);
		}
		if (x0 >= lw ()) th_fill (l, c, x0, vy - (oled () ? 0 : 1), x0, vy + (oled () ? 0 : 1), c_ink (), LV_OPA_COVER, 0);	// the event
	}
}

static void area_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), O = oled ();
	const lv_font_t *f = TH->f_micro;
	int fh = lv_font_get_line_height (f);
	if (!s_V.valid || (!s_V.name[0] && !s_V.length))
	{
		th_text (l, &c, 0, h / 2 - fh, w, O ? TH->f_body : &lv_font_montserrat_18, c_dim (), LV_TEXT_ALIGN_CENTER,
			 s_V.valid ? "NO TAKE: ARM, THEN PLAY ON THE RYTM" : "NO SONG ENGINE");
		return;
	}
	// rows
	int rr = O ? 10 : 18, rp = O ? 14 : 26, rs = O ? 8 : 14, rf = O ? 11 : 22, rt = O ? 8 : 16, mh = O ? 3 : 5;
	if (TH->dy) mh = O ? 4 : 7;			// 1024x600: taller mute rows
	int y = 0;
	unsigned sp = span ();
	// the ruler: bar numbers where they fit, a tick each bar
	int px_bar = (int) ((long long) SSG_BAR * (w - lw ()) / sp);
	int every = 1;
	while (px_bar * every < (O ? 21 : 44)) every *= 2;
	unsigned b0 = s_nStart / SSG_BAR;
	for (unsigned b = b0; ; b++)
	{
		int x = xof (b * SSG_BAR, w);
		if (x >= w) break;
		if (x < lw ()) continue;
		int major = b % every == 0;
		th_fill (l, &c, x, major ? 0 : rr - 2, x, rr - 1, major ? c_ink () : c_dim (), LV_OPA_COVER, 0);
		if (major)
		{
			char t[8];
			lv_snprintf (t, sizeof t, "%u", b + 1);
			int tw = th_text_width (f, t);
			if (x + 2 + tw <= w) th_text (l, &c, x + 2, O ? 0 : 1, tw, f, c_dim (), LV_TEXT_ALIGN_LEFT, t);	// (only whole: not cut at the edge)
		}
		// the bar lines, dotted, through the lanes
		if (major) th_fill (l, &c, x, rr, x, h - 1, O ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 40), LV_OPA_COVER, 0);
	}
	for (int k = 0; k < s_V.nmarks; k++)	// markers: a flag on the ruler
	{
		int x = xof (s_V.marks[k], w);
		if (x >= lw () && x < w) th_fill (l, &c, x, 0, x + (O ? 3 : 5), O ? 3 : 6, TH->accent, LV_OPA_COVER, 0);
	}
	y = rr + 1;
	// PTN: the patterns as blocks, alternately shaded, named
	lane_name (l, &c, y, rp, "PTN", c_ink ());
	for (int i = 0; i < s_V.nblocks; i++)
	{
		const ssg_block_t *b = &s_V.blocks[i];
		int x0 = xof (b->t0, w), x1 = xof (b->t1, w) - 1;
		if (x1 < lw () || x0 >= w) continue;
		if (x0 < lw ()) x0 = lw ();
		if (x1 > w - 1) x1 = w - 1;
		th_fill (l, &c, x0, y, x1, y + rp - 1, i & 1 ? (O ? 0x303030 : th_mix (c_bg (), c_dim (), 70)) : th_mix (c_bg (), c_rytm (), O ? 140 : 120),
			 LV_OPA_COVER, 0);
		th_fill (l, &c, x0, y, x0, y + rp - 1, c_ink (), LV_OPA_COVER, 0);
		char t[6];
		lv_snprintf (t, sizeof t, "%c%02d", 'A' + b->v / 16, b->v % 16 + 1);
		if (x1 - x0 > (O ? 21 : 36)) th_text (l, &c, x0 + 2, y + (rp - fh) / 2, x1 - x0 - 2, f, c_ink (), LV_TEXT_ALIGN_LEFT, t);
	}
	y += rp + 1;
	// SCN: scene blocks (teal), PRF: the PERF curve, BPM: the tempo line
	lane_name (l, &c, y, rs, "SCN", c_dim ());
	for (int i = 0; i < s_V.nscenes; i++)
	{
		int x0 = xof (s_V.scenes[i].t0, w), x1 = xof (s_V.scenes[i].t1, w) - 1;
		if (x1 < lw () || x0 >= w) continue;
		if (x0 < lw ()) x0 = lw ();
		if (x1 > w - 1) x1 = w - 1;
		th_fill (l, &c, x0, y, x1, y + rs - 1, c_side (), LV_OPA_COVER, 0);
	}
	y += rs + 1;
	lane_name (l, &c, y, rf, "PRF", c_dim ());
	curve (l, &c, w, y, rf, s_V.perf, s_V.nperf, 16383, c_rytm (), 1);
	y += rf + 1;
	lane_name (l, &c, y, rt, "BPM", c_dim ());
	if (s_V.ntempo)
	{
		unsigned lo = 0xFFFF, hi = 0;
		for (int i = 0; i < s_V.ntempo; i++) { lo = s_V.tempo[i].v < lo ? s_V.tempo[i].v : lo; hi = s_V.tempo[i].v > hi ? s_V.tempo[i].v : hi; }
		ssg_point_t p[SSG_TEMPO];
		for (int i = 0; i < s_V.ntempo; i++) p[i].t = s_V.tempo[i].t, p[i].v = (unsigned short) (hi > lo ? (s_V.tempo[i].v - lo) * 100 / (hi - lo) : 50);
		curve (l, &c, w, y, rt, p, s_V.ntempo, 100, c_ink (), 0);
	}
	else th_fill (l, &c, lw (), y + rt / 2, w - 1, y + rt / 2, O ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 60), LV_OPA_COVER, 0);
	y += rt + 1;
	// MUTE: 12 rows, lit while muted
	lane_name (l, &c, y, 12 * mh, "MUTE", c_ink ());
	th_fill (l, &c, lw (), y, w - 1, y + 12 * mh - 1, O ? 0x1C1C1C : th_mix (c_bg (), c_dim (), 25), LV_OPA_COVER, 0);
	for (int i = 0; i < s_V.nmutes; i++)
	{
		const ssg_block_t *b = &s_V.mutes[i];
		int x0 = xof (b->t0, w), x1 = xof (b->t1, w) - 1;
		if (x1 < lw () || x0 >= w || b->v >= 12) continue;
		if (x0 < lw ()) x0 = lw ();
		if (x1 > w - 1) x1 = w - 1;
		int yy = y + b->v * mh;
		th_fill (l, &c, x0, yy, x1, yy + mh - (O ? 1 : 2), c_ink (), LV_OPA_COVER, 0);
	}
	if (!O)
		for (int k = 0; k < 12; k += 4) th_text (l, &c, lw () - 22, y + k * mh - 2, 20, f, c_dim (), LV_TEXT_ALIGN_RIGHT, kTrk[k]);
	y += 12 * mh + 2;
	// the parameter lanes: the rest of the height, as many as fit
	int left = h - y, n = s_V.nlanes, lh = n ? left / n : 0, minh = O ? 9 : 18;
	if (lh < minh) { n = left / minh; lh = minh; }
	if (lh > (O ? 21 : 40)) lh = O ? 21 : 40;
	for (int i = 0; i < n && i < s_V.nlanes; i++)
	{
		const ssg_lane_t *L = &s_V.lane[i];
		uint32_t col = L->sidecar ? c_side () : c_rytm ();
		lane_name (l, &c, y, lh - 1, L->label, col);
		curve (l, &c, w, y, lh - 1, L->p, L->n, 16383, col, 1);
		y += lh;
	}
	// the playhead
	if (s_V.state == SSG_REC || s_V.state == SSG_REPLAYING)
	{
		int x = xof (s_V.playhead, w);
		if (x >= lw () && x < w) th_fill (l, &c, x, 0, x + (O ? 0 : 1), h - 1, s_V.state == SSG_REC ? TH->bad : c_ink (), LV_OPA_COVER, 0);
	}
	th_fill (l, &c, lw () - 1, rr, lw () - 1, h - 1, c_dim (), LV_OPA_COVER, 0);
}

static void area_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	if (code == LV_EVENT_PRESSED) { s_nDragX0 = p.x; s_nDragStart0 = s_nStart; return; }
	if (code != LV_EVENT_PRESSING || !kZoom[s_nZoom]) return;
	lv_area_t c;
	lv_obj_get_coords (s_Area, &c);
	int w = lv_area_get_width (&c) - lw ();
	long long d = (long long) (s_nDragX0 - p.x) * span () / (w > 0 ? w : 1);
	long long ns = (long long) s_nDragStart0 + d;
	long long mx = (long long) total () - (long long) span () / 2;
	if (ns > mx) ns = mx;
	if (ns < 0) ns = 0;
	if ((unsigned) ns != s_nStart)
	{
		s_nStart = (unsigned) ns;
		s_bFollow = 0;
		th_key_check (s_Key[K_FOLLOW], 0);
		lv_obj_invalidate (s_Area);
	}
}

// ---- the keys
static void keys_update (void)
{
	int st = s_V.state;
	th_key_text (s_Key[K_ARM], st == SSG_ARMING || st == SSG_ARMED ? "DISARM" : "ARM");
	th_key_led (s_Key[K_ARM], st == SSG_REC ? 15 : st == SSG_ARMED || st == SSG_ARMING ? 8 : 0);
	th_key_enable (s_Key[K_ARM], st == SSG_OFF || st == SSG_ARMING || st == SSG_ARMED);
	th_key_text (s_Key[K_REPLAY], st == SSG_READY || st == SSG_REPLAYING ? "OFF" : "REPLAY");
	th_key_led (s_Key[K_REPLAY], st == SSG_REPLAYING ? 31 : st == SSG_READY ? 24 : 0);
	th_key_enable (s_Key[K_REPLAY], (st == SSG_OFF && s_V.events) || st == SSG_READY || st == SSG_REPLAYING);
	th_key_enable (s_Key[K_PREV], st == SSG_OFF && s_V.takes > 0);
	th_key_enable (s_Key[K_NEXT], st == SSG_OFF && s_V.takes > 0);
	// UX review T14: UNDO REPLAY over < > while the engine offers it (after a replay, until an edit or STOP + 30 s)
	for (int k = K_PREV; k <= K_NEXT; k++)
		if (s_V.undo) lv_obj_add_flag (s_Key[k], LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Key[k], LV_OBJ_FLAG_HIDDEN);
	if (s_V.undo) lv_obj_remove_flag (s_Undo, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Undo, LV_OBJ_FLAG_HIDDEN);
	th_key_led (s_Key[K_CFG], s_V.config[0] ? 52 : 0);
}

static void dlg_show (int which)	// 0 none, 1 the confirmation, 2 the how-to
{
	if (!s_Dlg) return;
	if (!which) { lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN); return; }
	lv_obj_remove_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < 10; i++)
	{
		th_label_set (s_DlgText[i], "");
		th_label_color (s_DlgText[i], i == 9 ? (oled () ? TH->ink : TH->warn) : oled () ? TH->ink : TH->win_ink);
	}
	if (which == 1)
	{
		char b[80];
		th_label_set (s_DlgText[0], "REPLAY THIS TAKE?");
		lv_snprintf (b, sizeof b, "%s  (%u EVENTS)", s_V.name, s_V.events);
		th_label_set (s_DlgText[1], b);
		th_label_set (s_DlgText[3], "THIS OVERWRITES THE RYTM'S WORKING");
		th_label_set (s_DlgText[4], s_V.settings ? "KIT, PATTERN AND SETTINGS (NOT SAVED ONES)." : "KIT AND PATTERN (NOT THE SAVED ONES).");
		th_label_set (s_DlgText[5], "A BACKUP OF THEM IS WRITTEN FIRST");
		th_label_set (s_DlgText[6], "(/.rytm/songs/<project>/.backup-N.json).");
		th_label_set (s_DlgText[7], "STOP THE RYTM; THEN PLAY REPLAYS.");
		if (s_V.config[0])
		{
			lv_snprintf (b, sizeof b, "MIDI CONFIG: %s", s_V.config);
			th_label_set (s_DlgText[9], b);
		}
		th_key_text (s_DlgYes, "CONFIRM");
		th_key_kind (s_DlgYes, TH_KEY_DANGER | TH_KEY_FILL);	// it overwrites the working kit: filled (UX review T16)
		lv_obj_remove_flag (s_DlgNo, LV_OBJ_FLAG_HIDDEN);
	}
	else
	{
		// UX review T13: a checklist from what the Sidecar read of the Rytm's MIDI CONFIG - the wrong rows first (X),
		// the right ones dim (OK); every row with ? until it was read
		char b[48];
		int wr = s_V.cfg_wrong, line = 2;
		th_label_set (s_DlgText[0], kHowTo[0]);
		th_label_set (s_DlgText[1], oled () ? "GLOBAL > MIDI CONFIG, ON THE RYTM:" : "Set in GLOBAL > MIDI CONFIG on the Rytm (never by the Sidecar):");
		for (int pass = 0; pass < 2; pass++)
			for (int k = 0; k < SSG_CFG_ROWS; k++)
			{
				int bad = wr < 0 || (wr >> k & 1);
				if (bad != !pass || !s_V.cfg_row[k][0]) continue;
				lv_snprintf (b, sizeof b, "%s  %s", wr < 0 ? " ?" : bad ? " X" : "OK", s_V.cfg_row[k]);
				th_label_set (s_DlgText[line], b);
				th_label_color (s_DlgText[line], bad && wr >= 0 ? (oled () ? TH->accent_hi : TH->warn) : oled () ? (bad ? TH->ink : TH->dim) : (bad ? TH->win_ink : TH->win_dim));
				line++;
			}
		th_label_set (s_DlgText[9], wr < 0 ? "NOT READ YET: ARM ONCE TO READ IT" : wr ? "" : "ALL SET");
		th_label_color (s_DlgText[9], oled () ? TH->ink : TH->warn);
		th_key_text (s_DlgYes, "BACK");
		th_key_kind (s_DlgYes, TH_KEY_ACCENT);
		lv_obj_add_flag (s_DlgNo, LV_OBJ_FLAG_HIDDEN);
	}
}

static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud, st = s_V.state;
	switch (k)
	{
	case K_ARM: cmd (st == SSG_OFF ? "song=arm" : "song=disarm"); break;
	case K_REPLAY: cmd (st == SSG_OFF ? "song=replay" : "song=cancel"); break;
	case K_PREV: cmd ("song=prev"); break;
	case K_NEXT: cmd ("song=next"); break;
	case K_OUT: if (s_nZoom > 0) s_nZoom--; if (!kZoom[s_nZoom]) s_nStart = 0; lv_obj_invalidate (s_Area); break;
	case K_IN:
		if (s_nZoom + 1 < (int) (sizeof kZoom / sizeof kZoom[0])) s_nZoom++;
		if (s_bFollow && (s_V.state == SSG_REC || s_V.state == SSG_REPLAYING)) follow ();
		lv_obj_invalidate (s_Area);
		break;
	case K_FOLLOW: s_bFollow = !s_bFollow; th_key_check (s_Key[K_FOLLOW], s_bFollow); follow (); lv_obj_invalidate (s_Area); break;
	case K_CFG: sidecar_song_explain (!s_bExplain); break;
	case K_UNDO: if (s_V.undo) cmd ("song=undo"); break;
	}
}

static void dlg_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int yes = ud != 0;
	if (s_bExplain) { sidecar_song_explain (0); return; }
	cmd (yes ? "song=confirm" : "song=cancel");
}

void sidecar_song_create (void *parent, int y0, sidecar_song_cmd_t c)
{
	int O = oled ();
	s_Cmd = c;
	s_nGen = ~0u;
	s_bExplain = 0;
	memset (&s_V, 0, sizeof s_V);
	// 1024x600 (TH->dx, dy): the timeline gets the extra room, the keys wider, the overlay stays its size, centred
	const int dx = TH->dx, dy = TH->dy, W = O ? TH->w - 48 : TH->w, H = O ? TH->h : 428 + dy;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, W, H);
	s_Page = pg;
	static const char *const kKeys[8] = {"ARM", "REPLAY", "<", ">", "ZOOM -", "ZOOM +", "FOLLOW", "CFG"};	// (T12: zoom named)
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, W, 13);
		th_titlebar_set (s_TitleBar, "SONG", "", 0);
		s_Title = 0;
		s_Info = th_label (pg, 2, 16, TH->f_micro, TH->ink, "");
		s_Area = th_box (pg, 0, 28, W, 168 + dy);
		s_Msg = th_label (pg, 2, 198 + dy, TH->f_micro, TH->dim, "");
		for (int k = 0; k < 8; k++)
			s_Key[k] = th_key (pg, k * (44 + dx / 8) + (k > 3 ? 2 : 0), 209 + dy, 42 + dx / 8, 31, kKeys[k], TH_KEY | TH_KEY_SMALL, key_cb, (void *) (intptr_t) k);
		s_Undo = th_key (pg, 2 * (44 + dx / 8), 209 + dy, 86 + dx / 4 + 2, 31, "UNDO REPLAY", TH_KEY_ACCENT | TH_KEY_SMALL, key_cb, (void *) (intptr_t) K_UNDO);
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, 8, 6, 784 + dx, 352 + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "SONG");
		s_Info = th_label (pg, 316 + dx, 16, &lv_font_montserrat_16, TH->win_ink, "");
		s_Area = th_box (pg, 20, 44, 760 + dx, 290 + dy);
		s_Msg = th_label (pg, 24, 336 + dy, &lv_font_montserrat_14, TH->win_dim, "");
		for (int k = 0; k < 8; k++)
			s_Key[k] = th_key (pg, 12 + k * (97 + dx / 8), 366 + dy, 92 + dx / 8, 54, kKeys[k], k == K_REPLAY ? TH_KEY_ACCENT : TH_KEY, key_cb, (void *) (intptr_t) k);
		s_Undo = th_key (pg, 12 + 2 * (97 + dx / 8), 366 + dy, 189 + dx / 4, 54, "UNDO REPLAY", TH_KEY_ACCENT, key_cb, (void *) (intptr_t) K_UNDO);
	}
	lv_obj_set_width (s_Info, O ? 348 + dx : 460);
	if (!O) lv_obj_set_style_text_align (s_Info, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_width (s_Msg, (O ? 348 : 750) + dx);
	lv_label_set_long_mode (s_Msg, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_add_event_cb (s_Area, area_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_flag (s_Area, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Area, area_cb, LV_EVENT_ALL, 0);
	th_key_check (s_Key[K_FOLLOW], s_bFollow);
	lv_obj_add_flag (s_Undo, LV_OBJ_FLAG_HIDDEN);
	// the overlay: the confirmation / the how-to
	if (O)
	{
		s_Dlg = th_box (pg, 9 + dx / 2, 20 + dy / 2, 334, 216);
		lv_obj_set_style_bg_color (s_Dlg, lv_color_hex (TH->bg), 0);
		lv_obj_set_style_bg_opa (s_Dlg, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Dlg, lv_color_hex (TH->ink), 0);
		lv_obj_set_style_border_width (s_Dlg, 1, 0);
		for (int i = 0; i < 10; i++) s_DlgText[i] = th_label (s_Dlg, 6, 5 + i * 16 + (i ? 2 : 0), i ? TH->f_micro : TH->f_body, TH->ink, "");
		s_DlgYes = th_key (s_Dlg, 180, 178, 146, 32, "CONFIRM", TH_KEY_ACCENT, dlg_cb, (void *) 1);	// (filled while it overwrites: dlg_show)
		s_DlgNo = th_key (s_Dlg, 8, 178, 146, 32, "CANCEL", TH_KEY, dlg_cb, (void *) 0);
	}
	else
	{
		s_Dlg = th_box (pg, 120 + dx / 2, 40 + dy / 2, 560, 330);
		th_panel (s_Dlg, 0, 0, 560, 330, "", TH_PANEL_WINDOW);
		for (int i = 0; i < 10; i++)
			s_DlgText[i] = th_label (s_Dlg, 24, 18 + i * 26, i ? &lv_font_montserrat_16 : &lv_font_montserrat_20, i == 9 ? TH->warn : TH->win_ink, "");
		s_DlgYes = th_key (s_Dlg, 300, 270, 236, 50, "CONFIRM", TH_KEY_ACCENT, dlg_cb, (void *) 1);
		s_DlgNo = th_key (s_Dlg, 24, 270, 236, 50, "CANCEL", TH_KEY, dlg_cb, (void *) 0);
	}
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	keys_update ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_song_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_song_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
int sidecar_song_zoom (void) { return kZoom[s_nZoom]; }

void sidecar_song_explain (int on)
{
	s_bExplain = on;
	dlg_show (on ? 2 : s_V.state == SSG_CONFIRM ? 1 : 0);
}

static const char *state_text (int s)
{
	// UX review T12: state words, not "OFF" (off what?)
	static const char *const k[] = {"IDLE", "ARMING", "ARMED", "RECORDING", "SAVING", "REPLAY?", "RESTORING", "READY", "REPLAYING", "LOADING", "UNDOING"};
	return s >= 0 && s <= SSG_UNDOING ? k[s] : "?";
}

void sidecar_song_set (const sidecar_song_view_t *v)
{
	if (!s_Page) return;
	int lanes = v->gen != s_nGen;
	int head = v->playhead != s_V.playhead || v->state != s_V.state;
	int was = s_V.state;
	if (lanes) s_V = *v;
	else
	{	// the lanes unchanged: the header part only
		memcpy (&s_V, v, offsetof (sidecar_song_view_t, nblocks));
	}
	s_nGen = v->gen;
	char t[96], r[48];
	lv_snprintf (r, sizeof r, "%s%s%s", state_text (v->state), v->name[0] ? " " : "", v->name);
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "SONG", r, v->state == SSG_REC);
	else { lv_snprintf (t, sizeof t, "SONG  %s", r); th_label_set (s_Title, t); }
	unsigned at = v->state == SSG_REC || v->state == SSG_REPLAYING ? v->playhead : v->length;
	int pat = v->pattern < 0 ? 0 : v->pattern;
	lv_snprintf (t, sizeof t, "%u EV  BAR %u.%u  %c%02d  %d.%d BPM  TAKE %d/%d%s", v->events, at / SSG_BAR + 1, at % SSG_BAR / SSG_PPQ + 1,
		     'A' + pat / 16, pat % 16 + 1, v->bpm10 / 10, v->bpm10 % 10, v->take_i + 1, v->takes, v->recovered ? "  RECOVERED" : "");
	th_label_set (s_Info, t);
	if (v->config[0]) lv_snprintf (t, sizeof t, "MIDI CONFIG: SEE CFG  %s", v->msg);	// (the settings: CFG's checklist)
	else lv_snprintf (t, sizeof t, "%s", v->msg);
	th_label_set (s_Msg, t);
	keys_update ();
	if (v->state != was && !s_bExplain) dlg_show (v->state == SSG_CONFIRM ? 1 : 0);
	if (lanes || head)
	{
		follow ();
		lv_obj_invalidate (s_Area);
	}
}
