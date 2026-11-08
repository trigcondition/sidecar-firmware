// pi/ui/sidecar_sedit.c - see sidecar_sedit.h. A code-drawn waveform and theme keys; every mark through th_fill /
// th_text / th_frame, so the OLED theme keeps its dots and SIDECAR its cards.
#include "sidecar_sedit.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <string.h>

static const char *const kTrack[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
// the functions: their keys, and the names the console takes (core/samp/sedit.cpp kFnShort: no spaces, so a command
// never splits)
static const char *const kFnO[SSE_FN_N] = {"CROP", "NORM", "TRIM", "FD IN", "FD OUT", "REV", "GAIN+3", "GAIN-3"};
static const char *const kFnC[SSE_FN_N] = {"Crop", "Normalise", "Trim", "Fade in", "Fade out", "Reverse", "Gain +3 dB", "Gain -3 dB"};
static const char *const kFnBusy[SSE_FN_N] = {"CROP", "NORMALISE", "TRIM", "FADE IN", "FADE OUT", "REVERSE", "GAIN +3", "GAIN -3"};
static const char *const kFnCmd[SSE_FN_N] = {"CROP", "NORM", "TRIM", "FADEIN", "FADEOUT", "REV", "GAIN+", "GAIN-"};
static const char *const kKeyO[SSE_K_N] = {"ZOOM -", "ZOOM +", "PLAY", "UNDO", "SAVE", "CLOSE"};
static const char *const kKeyC[SSE_K_N] = {"Zoom -", "Zoom +", "Play", "Undo", "Save", "Close"};
enum { DLG_NONE, DLG_SAVE, DLG_DISCARD };
#define MIN_SPAN 64u	// ZOOM +'s smallest window (frames)

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Wave, *s_Fn[SSE_FN_N], *s_Key[SSE_K_N];
static lv_obj_t *s_Scrim, *s_Dlg, *s_DlgText, *s_DlgSub, *s_DlgKey[SSE_D_N];
static sidecar_sedit_view_t s_V;
static int s_bHave, s_nDlg;
static int s_nDrag = -1;		// the marker under the finger: 0 start, 1 end
static int s_bPanning, s_nShift;	// a pan in progress; the outline's shift in pixels (until the engine's window follows)
static lv_point_t s_P0;
static unsigned s_nGen = ~0u;
static void (*s_Cmd) (const char *cmd);
static char s_Last[64];

static int oled (void) { return TH->id == THEME_OLED; }
static int U (void) { return oled () ? 1 : 2; }
static const lv_font_t *f_small (void) { return oled () ? TH->f_micro : TH->f_small; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static int open_ (void) { return s_bHave && s_V.valid; }
static int have_wave (void) { return open_ () && s_V.frames > 0 && s_V.st != SSE_ST_LOADING && s_V.st != SSE_ST_ERROR; }
static int ready (void) { return open_ () && s_V.st == SSE_ST_READY; }

static void send (const char *t)
{
	lv_snprintf (s_Last, sizeof s_Last, "%s", t);
	if (s_Cmd) s_Cmd (t);
}
static void cmd2 (const char *fmt, unsigned a, unsigned b)
{
	char t[64];
	lv_snprintf (t, sizeof t, fmt, a, b);
	send (t);
}
const char *sidecar_sedit_last_command (void) { return s_Last; }
int sidecar_sedit_dragging (void) { return s_nDrag >= 0; }
void sidecar_sedit_set_command (void (*fn) (const char *cmd)) { s_Cmd = fn; }

// ---- the window [wa, wb) onto the waveform's columns 1 .. w - 2 (+ the pan's shift)
static unsigned span (void) { return s_V.wb > s_V.wa ? s_V.wb - s_V.wa : 1; }
static int x_of (unsigned f, int w)
{
	return 1 + (int) (((long long) f - s_V.wa) * (w - 3) / (long long) span ()) + s_nShift;
}
static unsigned f_of (int x, int w)
{
	long long f = s_V.wa + (long long) (x - 1 - s_nShift) * span () / (w > 3 ? w - 3 : 1);
	return f < 0 ? 0 : f > s_V.frames ? s_V.frames : (unsigned) f;
}
static void secs (unsigned frames, char *b, int cap)
{
	unsigned ms = (unsigned) ((unsigned long long) frames * 1000 / (s_V.rate ? s_V.rate : 48000));
	lv_snprintf (b, cap, "%u.%03u", ms / 1000, ms % 1000);
}
static void window (unsigned wa, unsigned len)	// the window asked for, kept inside the sample
{
	if (len > s_V.frames) len = s_V.frames;
	if (wa + len > s_V.frames) wa = s_V.frames - len;
	cmd2 ("sedit=window:%u:%u", wa, wa + len);
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
	const lv_font_t *fs = f_small (), *fb = O ? TH->f_body : TH->f_mid;
	int fh = lv_font_get_line_height (fs), bh = lv_font_get_line_height (fb);
	char t[64], s0[16], s1[16], s2[16];
	if (!have_wave ())
	{
		const char *m = !open_ () ? (O ? "NOTHING OPEN" : "Nothing open")
			      : s_V.st == SSE_ST_ERROR ? (s_V.msg[0] ? s_V.msg : "ERROR") : 0;
		if (!m) lv_snprintf (t, sizeof t, O ? "LOADING %d%%" : "Loading %d%%", s_V.progress), m = t;
		th_text (l, &c, 0, h / 2 - bh / 2, w, fb, c_dim (), LV_TEXT_ALIGN_CENTER, m);
		return;
	}
	int top = fh + 2 * u, bot = h - fh - 3 * u, mid = top + (bot - top) / 2, amp = (bot - top) / 2 - u;
	int xa = x_of (s_V.a, w), xb = x_of (s_V.b, w);
	{	// the region, lit behind the outline
		int x0 = xa < 1 ? 1 : xa, x1 = xb > w - 2 ? w - 2 : xb;
		if (x1 > x0) th_fill (l, &c, x0, top, x1, bot, O ? DOT_SHADE : th_mix (TH->win, TH->accent, 60), LV_OPA_COVER, 0);
	}
	th_fill (l, &c, 1, mid, w - 2, mid, O ? DOT_DOTTED : th_mix (TH->win, TH->win_dim, 90), LV_OPA_COVER, 0);
	for (int x = 1; x < w - 1 && s_V.ncols > 0; x++)	// the outline: the region in ink, the rest dim
	{
		int i = (int) ((long long) (x - 1 - s_nShift) * s_V.ncols / (w - 2));
		if (i < 0 || i >= s_V.ncols) continue;
		int a = mid - s_V.hi[i] * amp / 32768, b = mid - s_V.lo[i] * amp / 32768;
		if (a < top) a = top;
		if (b > bot) b = bot;
		if (b < a) b = a;
		th_fill (l, &c, x, a, x, b, x >= xa && x <= xb ? c_ink () : c_dim (), LV_OPA_COVER, 0);
	}
	if (s_V.playhead >= 0 && (unsigned) s_V.playhead >= s_V.wa && (unsigned) s_V.playhead < s_V.wb)
	{
		int x = x_of ((unsigned) s_V.playhead, w);
		if (x >= 1 && x < w - 1) th_fill (l, &c, x, top, x + u - 1, bot, TH->play, LV_OPA_COVER, 0);
	}
	for (int m = 0; m < 2; m++)	// the markers: a line and a flag at the top (the start's to the right, the end's to the left)
	{
		int x = m ? xb : xa;
		if (x < 1 || x > w - 2) continue;
		uint32_t mc = m == s_nDrag ? TH->accent_hi : TH->accent;
		th_fill (l, &c, x, top, x + u - 1, bot, mc, LV_OPA_COVER, 0);
		int fw = 5 * u + (m == s_nDrag ? 2 * u : 0), f0 = m ? x - fw : x, f1 = m ? x : x + fw;
		if (f0 < 1) f0 = 1;
		if (f1 > w - 2) f1 = w - 2;
		th_fill (l, &c, f0, top, f1, top + 4 * u, mc, LV_OPA_COVER, 0);
	}
	// the strip on top: the region in seconds; on the right the editor's message, else the format
	secs (s_V.a, s0, sizeof s0), secs (s_V.b, s1, sizeof s1), secs (s_V.b - s_V.a, s2, sizeof s2);
	lv_snprintf (t, sizeof t, O ? "%s-%s S  LEN %s" : "%s - %s s   length %s s", s0, s1, s2);
	th_text (l, &c, 3 * u, u, w / 2 + w / 8, fs, c_ink (), LV_TEXT_ALIGN_LEFT, t);
	if (s_V.msg[0]) th_text (l, &c, w / 2 + w / 8, u, w - w / 2 - w / 8 - 3 * u, fs, O ? TH->accent : TH->warn, LV_TEXT_ALIGN_RIGHT, s_V.msg);
	else
	{
		if (O) lv_snprintf (t, sizeof t, "%dCH %dBIT %u", s_V.ch, s_V.bits, s_V.rate);
		else lv_snprintf (t, sizeof t, "%s  %d-bit  %u Hz", s_V.ch > 1 ? "stereo" : "mono", s_V.bits, s_V.rate);
		th_text (l, &c, w / 2 + w / 8, u, w - w / 2 - w / 8 - 3 * u, fs, c_dim (), LV_TEXT_ALIGN_RIGHT, t);
	}
	// the bottom: the window and the zoom
	secs (s_V.wa, s0, sizeof s0), secs (s_V.wb, s1, sizeof s1), secs (s_V.frames, s2, sizeof s2);
	lv_snprintf (t, sizeof t, O ? "Z%d  %s-%s OF %s S" : "zoom %d   %s - %s of %s s", s_V.zoom, s0, s1, s2);
	th_text (l, &c, w / 4, h - fh - u, w * 3 / 4 - 3 * u, fs, c_dim (), LV_TEXT_ALIGN_RIGHT, t);
	if (s_V.st == SSE_ST_BUSY || s_V.st == SSE_ST_SAVING)	// a job: its name and progress across the middle
	{
		const char *n = s_V.st == SSE_ST_SAVING ? (O ? "SAVING" : "Saving") : kFnBusy[s_V.fn >= 0 && s_V.fn < SSE_FN_N ? s_V.fn : 0];
		lv_snprintf (t, sizeof t, "%s %d%%", n, s_V.progress);
		int tw = th_text_width (fb, t) + 8 * u, y0 = mid - bh / 2 - 2 * u, y1 = mid + bh / 2 + 4 * u;
		th_fill (l, &c, w / 2 - tw / 2, y0, w / 2 + tw / 2, y1, O ? TH->bg : TH->win, LV_OPA_COVER, 0);
		th_frame (l, &c, w / 2 - tw / 2, y0, w / 2 + tw / 2, y1, c_ink (), 1, O ? 0 : TH->radius);
		th_text (l, &c, w / 2 - tw / 2, y0 + 2 * u, tw, fb, c_ink (), LV_TEXT_ALIGN_CENTER, t);
		int pw = (tw - 8 * u) * s_V.progress / 100;
		if (pw > 0) th_fill (l, &c, w / 2 - tw / 2 + 4 * u, y1 - 2 * u, w / 2 - tw / 2 + 4 * u + pw, y1 - u, TH->accent, LV_OPA_COVER, 0);
	}
}
static int marker_near (int x, int w)	// the marker within reach of a finger at x (the nearer of the two)
{
	int best = -1, bd = oled () ? 8 : 18;
	for (int m = 0; m < 2; m++)
	{
		int d = x_of (m ? s_V.b : s_V.a, w) - x;
		d = d < 0 ? -d : d;
		if (d <= bd) bd = d, best = m;
	}
	return best;
}
static void inval_all (void);
static void wave_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	if (!have_wave ()) return;
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (s_Wave, &c);
	int w = lv_area_get_width (&c), x = p.x - c.x1;
	if (code == LV_EVENT_PRESSED)
	{
		s_P0 = p, s_bPanning = 0;
		s_nDrag = marker_near (x, w);
		lv_obj_invalidate (s_Wave);
	}
	else if (code == LV_EVENT_PRESSING)
	{
		int dx = p.x - s_P0.x;
		if (s_nDrag >= 0)	// shown at once; the engine's region follows the command at the release
		{
			unsigned f = f_of (x, w);
			if (s_nDrag == 0) s_V.a = f < s_V.b ? f : s_V.b - 1;
			else s_V.b = f > s_V.a ? f : s_V.a + 1;
			lv_obj_invalidate (s_Wave);
		}
		else if (s_bPanning || dx > 6 * U () || dx < -6 * U ())
		{
			s_bPanning = 1;		// the outline moved with the finger, as far as the sample goes
			long long wa = (long long) s_V.wa - (long long) dx * span () / (w > 3 ? w - 3 : 1);
			if (wa < 0) wa = 0;
			if (wa + span () > s_V.frames) wa = s_V.frames - span ();
			s_nShift = (int) (((long long) s_V.wa - wa) * (w - 3) / (long long) span ());
			lv_obj_invalidate (s_Wave);
		}
	}
	else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		if (s_nDrag >= 0 && code == LV_EVENT_RELEASED)
			cmd2 (s_nDrag ? "sedit=end:%u" : "sedit=start:%u", s_nDrag ? s_V.b : s_V.a, 0);
		else if (s_bPanning)
		{
			long long wa = (long long) s_V.wa - (long long) s_nShift * span () / (w > 3 ? w - 3 : 1);
			if (wa < 0) wa = 0;
			if (code == LV_EVENT_RELEASED && (unsigned) wa != s_V.wa) window ((unsigned) wa, span ());
			else s_nShift = 0;	// (nothing sent: nothing to wait for)
		}
		s_nDrag = -1, s_bPanning = 0;
		inval_all ();
	}
}

// ---- the question: SAVE + USE / SAVE / CANCEL, or DISCARD / CANCEL
static void dlg_show (int which)
{
	int O = oled ();
	s_nDlg = which;
	if (which == DLG_NONE)
	{
		lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	char b[80];
	if (which == DLG_SAVE)
	{
		th_label_set (s_DlgText, O ? "SAVE THE EDIT" : "Save the edit");
		lv_snprintf (b, sizeof b, "%s", s_V.has_track ? (O ? "AS A NEW FILE; USE: ON THE TRACK TOO" : "As a new file next to the original; Save + use: on the track too")
			     : (O ? "AS A NEW FILE NEXT TO THE ORIGINAL" : "As a new file next to the original"));
		th_label_set (s_DlgSub, b);
		th_key_text (s_DlgKey[SSE_D_OK], O ? "SAVE" : "Save");
		th_key_kind (s_DlgKey[SSE_D_OK], s_V.has_track ? TH_KEY : TH_KEY_ACCENT);
		if (s_V.has_track) lv_obj_remove_flag (s_DlgKey[SSE_D_USE], LV_OBJ_FLAG_HIDDEN);
		else lv_obj_add_flag (s_DlgKey[SSE_D_USE], LV_OBJ_FLAG_HIDDEN);
	}
	else
	{
		th_label_set (s_DlgText, O ? "DISCARD THE EDIT?" : "Discard the edit?");
		th_label_set (s_DlgSub, O ? "NOT SAVED: THE TRACK PLAYS THE ORIGINAL" : "It is not saved: the track plays the original again");
		th_key_text (s_DlgKey[SSE_D_OK], O ? "DISCARD" : "Discard");
		th_key_kind (s_DlgKey[SSE_D_OK], TH_KEY_DANGER | TH_KEY_FILL);
		lv_obj_add_flag (s_DlgKey[SSE_D_USE], LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_remove_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
}
static void dlg_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud, d = s_nDlg;
	dlg_show (DLG_NONE);
	if (k == SSE_D_CANCEL || !open_ ()) return;
	if (d == DLG_SAVE) send (k == SSE_D_USE && s_V.has_track ? "sedit=save:use" : "sedit=save");
	else if (d == DLG_DISCARD && k == SSE_D_OK) send ("sedit=close");
}

// ---- the keys
static void keys_update (void)
{
	int O = oled (), rd = ready (), wv = have_wave ();
	for (int i = 0; i < SSE_FN_N; i++) th_key_enable (s_Fn[i], rd);
	th_key_enable (s_Key[SSE_K_ZOOM_OUT], wv && span () < s_V.frames);
	th_key_enable (s_Key[SSE_K_ZOOM_IN], wv && span () / 2 >= MIN_SPAN);
	th_key_enable (s_Key[SSE_K_PLAY], rd && s_V.has_track);	// (the preview plays on the track's voice)
	th_key_enable (s_Key[SSE_K_UNDO], rd && s_V.undo);
	th_key_enable (s_Key[SSE_K_SAVE], rd);
	th_key_enable (s_Key[SSE_K_CLOSE], open_ ());
	const char *l = "SAMPLE EDIT";
	char r[80], nm[48];
	if (open_ ())
	{
		th_fit_middle (nm, sizeof nm, TH->f_body, s_V.name, O ? LU (180) + TH->dx / 2 : LU (380) + TH->dx / 2);
		const char *tr = s_V.has_track && s_V.track >= 0 && s_V.track < 12 ? kTrack[s_V.track] : "";
		if (O) lv_snprintf (r, sizeof r, "%s%s%s%s", nm, tr[0] ? " " : "", tr, s_V.dirty ? " *" : "");
		else lv_snprintf (r, sizeof r, "%s%s%s%s", nm, tr[0] ? "   track " : "", tr, s_V.dirty ? "   (not saved)" : "");
	}
	else r[0] = 0;
	if (O) th_titlebar_set (s_TitleBar, l, r, 0);
	else { char t[112]; lv_snprintf (t, sizeof t, "%s   %s", l, r); th_label_set (s_Title, t); }
}
static void inval_all (void)
{
	lv_obj_invalidate (s_Wave);
	keys_update ();
}
static void fn_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	if (!ready () || k < 0 || k >= SSE_FN_N) return;
	char t[32];
	lv_snprintf (t, sizeof t, "sedit=apply:%s", kFnCmd[k]);
	send (t);
}
static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud;
	if (!open_ ()) return;
	unsigned sp = span ();
	switch (k)
	{
	case SSE_K_ZOOM_OUT:
	case SSE_K_ZOOM_IN:	// half / twice the window, around its middle
	{
		if (!have_wave ()) break;
		unsigned mid = s_V.wa + sp / 2, ns = k == SSE_K_ZOOM_IN ? sp / 2 : sp * 2;
		if (ns < MIN_SPAN) ns = MIN_SPAN;
		if (ns > s_V.frames) ns = s_V.frames;
		s_nShift = 0;
		window (mid > ns / 2 ? mid - ns / 2 : 0, ns);
		break;
	}
	case SSE_K_PLAY: if (ready ()) send ("sedit=preview"); break;
	case SSE_K_UNDO: if (ready ()) send ("sedit=undo"); break;
	case SSE_K_SAVE: if (ready ()) dlg_show (DLG_SAVE); break;
	default:		// CLOSE: an edit not saved is asked about first (the Rytm's NO asks the same)
		if (s_V.dirty) dlg_show (DLG_DISCARD);
		else send ("sedit=close");
		break;
	}
	inval_all ();
}

// ---- the page
void sidecar_sedit_create (void *parent, int y0)
{
	int O = oled ();
	int pw = th_page_w (), ph = O ? TH->h : LU (428) + TH->dy;
	int x0 = O ? 0 : LU (8), g = TH->gap, kh = LU (O ? 22 : 52) + TH->dy / 8;
	int ky2 = ph - kh - (O ? 0 : LU (4)), ky1 = ky2 - kh - g;
	int top = O ? LU (15) : LU (38);
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph);
	s_Page = pg;
	s_nGen = ~0u, s_bHave = 0, s_nDlg = DLG_NONE, s_nDrag = -1, s_bPanning = 0, s_nShift = 0;
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, LU (13));
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		s_Title = th_label (pg, LU (12), LU (8), TH->f_mid, TH->ink, "SAMPLE EDIT");
	}
	s_Wave = th_box (pg, x0, top, pw - 2 * x0, ky1 - LU (O ? 3 : 8) - top);
	lv_obj_add_flag (s_Wave, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Wave, wave_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Wave, wave_cb, LV_EVENT_ALL, 0);
	int rw = pw - 2 * x0, kw = (rw - (SSE_K_N - 1) * g) / SSE_K_N;
	for (int i = 0; i < SSE_K_N; i++)
	{
		int kind = i == SSE_K_SAVE ? TH_KEY_ACCENT : i == SSE_K_PLAY ? TH_KEY_FUNC : TH_KEY;
		s_Key[i] = th_key (pg, x0 + i * (kw + g), ky1, kw, kh, O ? kKeyO[i] : kKeyC[i], kind, key_cb, (void *) (intptr_t) i);
		if (!O) th_key_font (s_Key[i], TH->f_body);
	}
	int fw = (rw - (SSE_FN_N - 1) * g) / SSE_FN_N;
	for (int i = 0; i < SSE_FN_N; i++)
	{
		s_Fn[i] = th_key (pg, x0 + i * (fw + g), ky2, fw, kh, O ? kFnO[i] : kFnC[i], O ? TH_KEY | TH_KEY_SMALL : TH_KEY, fn_cb, (void *) (intptr_t) i);
		if (!O) th_key_font (s_Fn[i], TH->f_body);
	}
	// the question: over a layer that takes every other tap
	s_Scrim = th_box (pg, 0, 0, pw, ph);
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_CLICKABLE);
	if (!O)
	{
		lv_obj_set_style_bg_color (s_Scrim, lv_color_hex (0x000000), 0);
		lv_obj_set_style_bg_opa (s_Scrim, LV_OPA_50, 0);
	}
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	int dw = O ? LU (300) + TH->dx / 2 : LU (600) + TH->dx / 2, dh = O ? LU (130) + TH->dy / 2 : LU (260) + TH->dy / 2;
	s_Dlg = th_box (pg, (pw - dw) / 2, (ph - dh) / 2, dw, dh);
	if (O)
	{
		lv_obj_set_style_bg_color (s_Dlg, lv_color_hex (TH->bg), 0);
		lv_obj_set_style_bg_opa (s_Dlg, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Dlg, lv_color_hex (TH->ink), 0);
		lv_obj_set_style_border_width (s_Dlg, 1, 0);
	}
	else th_panel (s_Dlg, 0, 0, dw, dh, "", TH_PANEL_WINDOW);
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_CLICKABLE);
	int tx = O ? LU (8) : LU (24);
	s_DlgText = th_label (s_Dlg, tx, LU (O ? 8 : 20), O ? TH->f_body : TH->f_mid, c_ink (), "");
	lv_obj_set_width (s_DlgText, dw - 2 * tx);
	s_DlgSub = th_label (s_Dlg, tx, LU (O ? 26 : 56), O ? TH->f_small : TH->f_body, c_dim (), "");
	lv_obj_set_width (s_DlgSub, dw - 2 * tx);
	static const char *const kDO[SSE_D_N] = {"SAVE+USE", "SAVE", "CANCEL"}, *const kDC[SSE_D_N] = {"Save + use", "Save", "Cancel"};
	int dkh = LU (O ? 26 : 56) + TH->dy / 8, dkw = (dw - 2 * tx - 2 * g) / 3, dky = dh - dkh - LU (O ? 8 : 20);
	for (int i = 0; i < SSE_D_N; i++)
	{
		s_DlgKey[i] = th_key (s_Dlg, tx + i * (dkw + g), dky, dkw, dkh, O ? kDO[i] : kDC[i], i == SSE_D_USE ? TH_KEY_ACCENT : TH_KEY,
				      dlg_cb, (void *) (intptr_t) i);
		if (!O) th_key_font (s_DlgKey[i], TH->f_body);
	}
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	keys_update ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}
void sidecar_sedit_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
	else
	{
		lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
		dlg_show (DLG_NONE);	// (a question is not left for the next visit)
	}
}
int sidecar_sedit_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }
void sidecar_sedit_set (const sidecar_sedit_view_t *v)
{
	if (!s_Page) return;
	if (s_bHave && v->gen == s_nGen && s_nDrag < 0) return;
	int drag = s_nDrag;
	unsigned ka = s_V.a, kb = s_V.b;	// (the marker being dragged stays where the finger is)
	if (s_bHave && (v->wa != s_V.wa || v->wb != s_V.wb) && !s_bPanning) s_nShift = 0;	// the engine's window followed the pan
	s_V = *v;
	if (drag == 0) s_V.a = ka < s_V.b ? ka : s_V.b - 1;
	if (drag == 1) s_V.b = kb > s_V.a ? kb : s_V.a + 1;
	s_nGen = v->gen, s_bHave = 1;
	if (!open_ ()) dlg_show (DLG_NONE);
	inval_all ();
}
int sidecar_sedit_hit (int what, int i, int *x, int *y)
{
	lv_area_t c;
	if (!sidecar_sedit_shown ()) return 0;
	switch (what)
	{
	case 0:
	{
		if (i < 0 || i > 1 || !have_wave ()) return 0;
		lv_obj_get_coords (s_Wave, &c);
		int w = lv_area_get_width (&c), mx = x_of (i ? s_V.b : s_V.a, w);
		if (mx < 1 || mx > w - 2) return 0;
		*x = c.x1 + mx, *y = (c.y1 + c.y2) / 2;
		return 1;
	}
	case 1:
	case 2:
	case 3:
	{
		lv_obj_t *k = what == 1 ? (i >= 0 && i < SSE_FN_N ? s_Fn[i] : 0) : what == 2 ? (i >= 0 && i < SSE_K_N ? s_Key[i] : 0)
			    : (i >= 0 && i < SSE_D_N && s_nDlg ? s_DlgKey[i] : 0);
		if (!k || lv_obj_has_flag (k, LV_OBJ_FLAG_HIDDEN)) return 0;
		lv_obj_get_coords (k, &c);
		*x = (c.x1 + c.x2) / 2, *y = (c.y1 + c.y2) / 2;
		return 1;
	}
	default:
		lv_obj_get_coords (s_Wave, &c);
		*x = c.x1 + lv_area_get_width (&c) * i / 100, *y = (c.y1 + c.y2) / 2;
		return 1;
	}
}
