// pi/ui/sidecar_ui.c - see sidecar_ui.h. The home page (the 8 voices, the engine's state, the project's user machines, the
// Sidecar's address or a touch test), the settings pages (Sound: the web page's controls; Send FX: read only + kill
// tails; Display: UI style, screensaver, touch test, brightness; About), and the frame around the pattern pages (SEQ,
// RYTM: sidecar_seqed.c, sidecar_rytmpat.c). Each page is laid out per theme (docs/UI-THEMES.md):
//   OLED     400x240 dots (512x300 on 1024x600): a dithered title bar a page, the content left, a fixed navigation
//            column right (HOME / SEQ / PAT / SONG / SET)
//   CLASSIC  800x480 (or 1024x600): the title bar with Seq and Settings, the pages below it (the layout of before),
//            drawn as the Rytm MKII's hardware
// The 1024x600 layouts are the 800x480 ones with what the screen adds (TH->dx, dy) given to the panels that grow.
// Both build the same objects (s_*), so the status update below is one code path. Every control sends a console
// command through the caller (sidecar_ui_set_command) and shows the state the caller reads back.
#include "touch_cal.h"
#include "sidecar_ui.h"
#include "sidecar_theme.h"
#include "sidecar_vlabel.h"
#include "lvgl.h"			// the LVGL tree's root (pi/lvgl-test/Makefile, host/Makefile)
#include "src/misc/lv_area_private.h"	// lv_area_intersect, l->_clip_area (the link banner)
#include "sidecar_wordmark.h"		// "Sidecar" in the docs site's wordmark face (wordmark.py): CLASSIC's title bar
#include "sidecar_seqed.h"		// page 4: the touch pattern editor (docs/TOUCH-EDITOR.md)
#include "sidecar_rytmpat.h"		// page 5: the Rytm pattern, read only
#include "sidecar_wgview.h"		// page 7: WAVETABLE / GRANULAR, the machine view
#include "sidecar_voice.h"		// page 8: VOICE, the Rytm's selected track (rui1036b: UP + T4)
#include "sidecar_insfx.h"		// page 9: INSERT FX, a voice's inserts and sends (fx-ins)
#include "sidecar_song.h"		// page 10: SONG, SONG RECORD's timeline (core/song)
#include "sidecar_rec.h"		// page 11: REC, the Overbridge multitrack recorder (UP + T7, the home page's REC status)
#include "sidecar_scope.h"		// page 12: SCOPE, an Overbridge channel's trace and the meters (UP + T9, VOICE's scope)
#include "sidecar_status.h"		// page 13: SIDECAR STATUS (UP + T10, the home page's ENGINE / Status panel)
#include "../../core/ui/page_keys.h"	// the device's shortcuts (the captions, the map): one table, device-agnostic
#include "sidecar_project.h"		// page 14: PROJECT (project bundles: a tap on the home page's PROJECT panel)
#include "sidecar_chop.h"		// page 17: CHOP, the slice machine's page (a CHOP voice's VOICE page; core/chop)
					// page 15: POOL, the three pool lists (sidecar_wgview.c's POOL page: Settings > Pool, ALL POOLS)
#include <string.h>
#include <stdlib.h>

enum { SEG_TAB, SEG_NOTES, SEG_OUT, SEG_MAC, SEG_VOICE, SEG_MACH, SEG_DRONE, SEG_SAVER, SEG_STYLE, SEG_LOOK, SEG_CLOCK, SEG_N };
enum { PG_HOME, PG_SOUND, PG_FX, PG_ABOUT, PG_SEQ, PG_RYTM, PG_DISPLAY, PG_MACH, PG_VOICE, PG_INS, PG_SONG, PG_REC, PG_SCOPE, PG_STATUS, PG_PROJECT, PG_POOL,
       PG_SERVICE, PG_CHOP, PG_N };	// SERVICE (UX review T18): FIFO and the test drone, out of the musician's Sound page; the tab is called System: the CPU clock too

// the settings tabs; Pool: the POOL page (the three lists, sidecar_wgview.h), a page of its own
#define N_TABS 6
static const char *const kTabMap[] = {"Sound", "Send FX", "Display", "Pool", "System", "About", ""};
static const int kTabPage[N_TABS] = {PG_SOUND, PG_FX, PG_DISPLAY, PG_POOL, PG_SERVICE, PG_ABOUT};
static const char *const kOnOff[] = {"Off", "On", ""};
static const char *const kOutMap[] = {"Main", "Tracks", ""};
static const char *const kVoiceMap[] = {"BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", ""};
static const char *const kVoiceMap2[] = {"BD", "SD", "RS/CP", "BT", "\n", "LT", "MT/HT", "CH/OH", "CY/CB", ""};
#define N_MACH 7	// MD in DIGIDRUM's (DD's) place since 2026-10-29
static const char *const kMachMap[] = {"PLAITS", "JUNO", "FM", "DAISY", "DSPS", "GRAIN", "MD", ""};
static const char *const kMachMap2[] = {"PLAITS", "JUNO", "FM", "DAISY", "\n", "DSPS", "GRAIN", "MD", ""};
static const char *const kMachCmd[N_MACH] = {"plaits", "juno", "fm", "daisy", "dsps", "grain", "md"};	// notemach=V:NAME
static const char *const kDroneMap[] = {"Off", "This voice", "All", ""};
static const char *const kDroneMap2[] = {"Off", "Voice", "All", ""};
static const char *const kSaverMap[] = {"Off", "15 min", "1 h", "4 h", ""};
static const int kSaverMin[4] = {0, 15, 60, 240};		// screensaver=MIN
static const char *const kClockMap[] = {"1500 MHz", "1200 MHz", "1000 MHz", ""};	// Settings > System: cpumhz= (pi/engine/cpuclock.h)
static const int kClockMHz[3] = {1500, 1200, 1000};
static const char *const kStyleMap[] = {"OLED", "Classic", ""};
static const char *const kLookMap[] = {"Studio", "Stage", ""};	// L1: the OLED look
static const char *const kSendName[SIDECAR_UI_SENDS] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
static const char *const kAboutName[8] = {"Web page", "Address", "IP", "Web server", "Link", "USB-C", "Version", "Libraries"};

// ---- the objects (both themes build all of them)
static lv_obj_t *s_Title, *s_Rytm, *s_Blocks, *s_Underruns, *s_Note, *s_Rec;
static lv_obj_t *s_NowPat, *s_NowTr, *s_NowRec, *s_Health;	// HOME's NOW panel (UX review T1)
static lv_obj_t *s_Project, *s_USlot[5];
static char s_UText[5][24];
static int s_UState[5] = {-1, -1, -1, -1, -1};
static uint32_t s_UErrT0[5];
static lv_timer_t *s_UTimer;

static lv_obj_t *s_Track[SIDECAR_UI_TRACKS];
static lv_obj_t *s_Load[SIDECAR_UI_CORES];
static lv_obj_t *s_Touch, *s_Taps, *s_SliderVal, *s_Dot;
static lv_obj_t *s_Touch2, *s_Taps2;
static lv_obj_t *s_Map;				// the UP + T map (N3)
static lv_obj_t *s_Stale;				// F2: a mirror page while the Rytm is not connected
static int s_nMap;			// Settings > Display's touch test (UX review T1)
static lv_obj_t *s_TouchPanel, *s_InfoPanel, *s_Info[4];
// UX review N1: one navigation for both themes, mirroring the device's page requests (each key captioned with its key
// from the device's table, core/ui/page_keys.h: the Rytm's UP + T1..T13). rui1052: the table's first eight rows are
// this column, in this order, so the captions read T1..T8 top to bottom. OLED: the column at the right edge; CLASSIC: a
// row in the title bar.
enum { NAV_HOME, NAV_SEQ, NAV_PAT, NAV_VOICE, NAV_FX, NAV_SONG, NAV_REC, NAV_SET, NAV_N };
static const char *const kNavName[NAV_N] = {"HOME", "SEQ", "PAT", "VOICE", "FX", "SONG", "REC", "SET"};
static const char *const kNavNameC[NAV_N] = {"Home", "Seq", "Pat", "Voice", "FX", "Song", "Rec", "Set"};
static const char *const kNavPage[NAV_N] = {"HOME", "SEQ", "PAT", "VOICE", "INSERT FX", "SONG", "REC", "SET"};	// the table's names
static const char *nav_t (int i)		// its caption ("T11"; "" when the device has no key for it)
{
	static char b[NAV_N][8];
	const sc_page_key_t *k = sc_page_of_name (kNavPage[i]);
	if (k) lv_snprintf (b[i], sizeof b[i], "%s%d", sc_keymap ()->map_key, k->code); else b[i][0] = 0;
	return b[i];
}
// N3: the map's text from the same table (the device's modifier + each page request, then its combos with a map
// text), two columns (two labels: the CLASSIC font is proportional). Each entry's first word is the key, as on the
// Rytm; CLASSIC writes the page's words in its case ("T6 Insert FX"): a word of up to three letters is an abbreviation
// and stays (SEQ, PAT, FX), a plural of one gets a small s (LFOs).
static void map_word (char *d, const char *s, int classic)
{
	for (int w = 0; *s; w++)
	{
		int len = 0;
		while (s[len] && s[len] != ' ') len++;
		int stem = len >= 2 && s[len - 1] == 'S' && len - 1 <= 3 ? len - 1 : len;
		for (int k = 0; k < len; k++)
		{
			int low = classic && w && k && s[k] >= 'A' && s[k] <= 'Z' && (len > 3 ? stem == len || k == len - 1 : 0);
			d[k] = low ? (char) (s[k] + 32) : s[k];
		}
		d += len, s += len;
		if (*s == ' ') *d++ = *s++;
	}
	*d = 0;
}
static void map_cols (int classic, char *c1, char *c2, int cap)	// c1: the title and the left column; c2 the right
{
	const sc_keymap_t *m = sc_keymap ();
	char col[24][24];
	int n = 0;
	for (int i = 0; i < m->npages && n < 24; i++)
	{
		char b[24];
		lv_snprintf (b, sizeof b, "%s%d %s", m->map_key, m->pages[i].code, m->pages[i].name);
		map_word (col[n++], b, classic);
	}
	for (int i = 0; i < m->ncombos && n < 24; i++) if (m->combos[i].map) map_word (col[n++], m->combos[i].map, classic);
	int rows = (n + 1) / 2, a = lv_snprintf (c1, cap, classic ? "%s + key on the Rytm\n" : "%s + KEY ON THE RYTM\n", m->modifier);
	int b = lv_snprintf (c2, cap, "\n");
	for (int r = 0; r < rows; r++)
	{
		a += lv_snprintf (c1 + a, cap - a, "\n%s", col[r]);
		if (r + rows < n) b += lv_snprintf (c2 + b, cap - b, "\n%s", col[r + rows]);
	}
}
static lv_obj_t *s_Home, *s_Pages, *s_Pg[PG_N], *s_Nav[NAV_N], *s_SetTitle;
static lv_obj_t *s_Seg[SEG_N], *s_FifoVal, *s_DroneInfo, *s_FxName[2], *s_Bar[2][SIDECAR_UI_SENDS], *s_Kill;
static lv_obj_t *s_About[8], *s_Bright, *s_BrightVal, *s_Dim, *s_Power;
static char s_Banner[96];				// the warning banner's text (s_Power draws it itself: tests read it here)

// ---- state (kept across a theme switch)
static int s_nStyle = THEME_OLED, s_nBuilt, s_nStyleSeen = -1;
static int s_PhysW, s_PhysH;				// the screen's size (the display as the caller created it)
static int s_nNavLit = -1;				// the navigation key lit now (-1: none)
static int s_nTouchShown = 1, s_nTouchSeen = -1, s_bTouchDown;	// the touch test shown now; the caller's last value (-1: none yet)
static unsigned s_nTaps;
static int s_nRytm = -1, s_nPage, s_nLast = PG_SOUND, s_nPoolFrom = PG_HOME;
static sidecar_ui_command_t s_Cmd;
static void *s_CmdParam;
static sidecar_ui_status_t s_St;			// the last state given: the voice selector and a rebuild re-read it
static int s_bHaveSt;
static int s_nVoice;					// the voice the machine and drone controls act on
static unsigned s_nFifo = 96;
static int s_nDim = 100;
static int s_Sel[SEG_N];				// the checked item of each segmented control, -2: not set yet

static int oled (void) { return s_nStyle == THEME_OLED; }
static void command (const char *c) { if (s_Cmd) s_Cmd (c, s_CmdParam); }
static void set_text (lv_obj_t *l, const char *t) { if (l) th_label_set (l, t); }

static lv_obj_t *label (lv_obj_t *parent, int x, int y, const lv_font_t *f, uint32_t color, const char *text)
{
	return th_label (parent, x, y, f, color, text);
}

static lv_obj_t *label_w (lv_obj_t *parent, int x, int y, int w, const lv_font_t *f, uint32_t color, const char *text)
{
	lv_obj_t *l = th_label (parent, x, y, f, color, text);
	lv_obj_set_width (l, w);
	lv_label_set_long_mode (l, LV_LABEL_LONG_MODE_CLIP);
	return l;
}

static void seg_set (int id, int idx)		// only a change reaches LVGL
{
	if (!s_Seg[id] || s_Sel[id] == idx) return;
	th_seg_set (s_Seg[id], idx);
	s_Sel[id] = idx;
}

static void seg_cb (lv_obj_t *m, int i, void *ud);
static lv_obj_t *seg (lv_obj_t *parent, int id, int x, int y, int w, int h, const char *const *map, int kind)
{
	lv_obj_t *m = th_seg (parent, x, y, w, h, map, kind, seg_cb, (void *) (intptr_t) id);
	s_Seg[id] = m;
	s_Sel[id] = -2;
	return m;
}

// ---- the touch test

static void tap_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	char b[24];
	lv_snprintf (b, sizeof b, oled () ? "TAPS %u" : "taps: %u", ++s_nTaps);
	set_text (s_Taps, b);
	set_text (s_Taps2, b);
}

static void slider_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o; (void) ud;
	char b[8];
	lv_snprintf (b, sizeof b, "%d", v & 0xFFFF);
	set_text (s_SliderVal, b);
}

static void touch_view (int shown)
{
	s_nTouchShown = shown;
	if (shown) lv_obj_remove_flag (s_TouchPanel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_TouchPanel, LV_OBJ_FLAG_HIDDEN);
	if (shown) lv_obj_add_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN);
	if (!shown) lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
}

static void hide_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	touch_view (0);
	command ("touchtest=0");
}

// ---- the touch calibration (Settings > Display > CALIBRATE; touch_cal.h): a cross at each of the 4 targets in turn, the
// raw values at each release taken; then "touchcal=X0,X1,Y0,Y1,SWAP" (stored in /.rytm/display.txt). Over everything
// (the top layer, clickable: no control below gets the touches). A release on the CANCEL box in the middle ends it.

static lv_obj_t *s_CalOv, *s_CalH, *s_CalV, *s_CalText, *s_CalCancel, *s_CalState;
static int s_nCalStep = -1, s_bCalDown, s_CalRaw[TOUCH_CAL_POINTS][2], s_nCalCal = -1;

static void cal_cancel_area (int *x, int *y, int *w, int *h)	// UI coordinates
{
	*w = oled () ? 80 : 180; *h = oled () ? 24 : 56;
	*x = (TH->w - *w) / 2; *y = (TH->h - *h) / 2 + (oled () ? 30 : 70);
}

static void cal_show (const char *msg)
{
	int fx, fy, r = oled () ? 12 : 24;
	touch_cal_target (s_nCalStep, &fx, &fy);
	int cx = TH->w * fx / 1000, cy = TH->h * fy / 1000;
	lv_obj_set_pos (s_CalH, cx - r, cy);
	lv_obj_set_pos (s_CalV, cx, cy - r);
	char b[96];
	lv_snprintf (b, sizeof b, oled () ? "%sTOUCH THE CROSS (%d OF 4)" : "%sTouch the cross (%d of 4)", msg, s_nCalStep + 1);
	set_text (s_CalText, b);
}

static void cal_end (void)
{
	s_nCalStep = -1;
	s_bCalDown = 0;
	if (s_CalOv) lv_obj_delete (s_CalOv);
	s_CalOv = 0;
}

static void cal_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	if (s_CalOv) return;
	lv_obj_t *top = lv_layer_top ();
	s_CalOv = th_box (top, 0, 0, TH->w, TH->h);
	lv_obj_set_style_bg_color (s_CalOv, lv_color_hex (oled () ? 0x000000 : TH->bg), 0);
	lv_obj_set_style_bg_opa (s_CalOv, LV_OPA_COVER, 0);
	lv_obj_add_flag (s_CalOv, LV_OBJ_FLAG_CLICKABLE);		// the touches stop here
	int r = oled () ? 12 : 24, t = oled () ? 1 : 2;
	s_CalH = th_box (s_CalOv, 0, 0, 2 * r + 1, t);
	s_CalV = th_box (s_CalOv, 0, 0, t, 2 * r + 1);
	lv_obj_set_style_bg_color (s_CalH, lv_color_hex (TH->ink), 0);
	lv_obj_set_style_bg_opa (s_CalH, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color (s_CalV, lv_color_hex (TH->ink), 0);
	lv_obj_set_style_bg_opa (s_CalV, LV_OPA_COVER, 0);
	const lv_font_t *f = oled () ? TH->f_body : &lv_font_montserrat_20;
	int tw = oled () ? 260 : 560;
	s_CalText = label_w (s_CalOv, (TH->w - tw) / 2, TH->h / 2 - (oled () ? 30 : 60), tw, f, TH->ink, "-");
	lv_obj_set_style_text_align (s_CalText, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *l = label_w (s_CalOv, (TH->w - tw) / 2, TH->h / 2 - (oled () ? 14 : 28), tw, oled () ? TH->f_micro : &lv_font_montserrat_14,
			       TH->dim, oled () ? "PRECISELY, WITH A FINGERTIP OR A STYLUS" : "Precisely, with a fingertip or a stylus");
	lv_obj_set_style_text_align (l, LV_TEXT_ALIGN_CENTER, 0);
	int cx, cy, cw, ch;
	cal_cancel_area (&cx, &cy, &cw, &ch);
	s_CalCancel = th_key (s_CalOv, cx, cy, cw, ch, oled () ? "CANCEL" : "Cancel", TH_KEY, 0, 0);
	s_nCalStep = 0;
	s_bCalDown = 0;
	cal_show ("");
}

static void calreset_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	command ("touchcal=off");
}

// a touch while calibrating (sidecar_ui_touch): 1 taken
static int cal_touch (int pressed, int raw_x, int raw_y, int x, int y)
{
	if (s_nCalStep < 0) return 0;
	if (pressed) { s_bCalDown = 1; s_CalRaw[s_nCalStep][0] = raw_x; s_CalRaw[s_nCalStep][1] = raw_y; return 1; }
	if (!s_bCalDown) return 1;
	s_bCalDown = 0;
	int ux = x, uy = y, cx, cy, cw, ch;
	sidecar_ui_to_ui (&ux, &uy);
	cal_cancel_area (&cx, &cy, &cw, &ch);
	if (ux >= cx && ux < cx + cw && uy >= cy && uy < cy + ch) { cal_end (); return 1; }
	s_CalRaw[s_nCalStep][0] = raw_x;				// the raw values at the release
	s_CalRaw[s_nCalStep][1] = raw_y;
	if (++s_nCalStep < TOUCH_CAL_POINTS) { cal_show (""); return 1; }
	touch_cal_t c;
	if (!touch_cal_compute ((const int (*)[2]) s_CalRaw, &c))
	{
		s_nCalStep = 0;
		cal_show (oled () ? "NO FIT. AGAIN: " : "The points did not fit. Again: ");
		return 1;
	}
	char b[80] = "touchcal=";
	touch_cal_format (&c, b + 9, (int) sizeof b - 9);
	cal_end ();
	command (b);
	return 1;
}

// ---- pages

static void stale_view (void);
static void show_page (int page)
{
	s_nPage = page;
	int set = page == PG_SOUND || page == PG_FX || page == PG_ABOUT || page == PG_DISPLAY || page == PG_SERVICE;
	if (page) lv_obj_add_flag (s_Home, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Home, LV_OBJ_FLAG_HIDDEN);
	if (set) lv_obj_remove_flag (s_Pages, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Pages, LV_OBJ_FLAG_HIDDEN);
	sidecar_seqed_show (page == PG_SEQ);
	sidecar_rytmpat_show (page == PG_RYTM);
	if (sidecar_wgview_pool_page_shown () != (page == PG_POOL)) sidecar_wgview_pool_page (page == PG_POOL);
	sidecar_wgview_show (page == PG_MACH || page == PG_POOL);
	sidecar_voice_show (page == PG_VOICE);
	sidecar_insfx_show (page == PG_INS);
	sidecar_song_show (page == PG_SONG);
	sidecar_rec_show (page == PG_REC);
	sidecar_scope_show (page == PG_SCOPE);
	sidecar_status_show (page == PG_STATUS);
	sidecar_project_show (page == PG_PROJECT);
	sidecar_chop_show (page == PG_CHOP);
	for (int i = 0; i < PG_N; i++)
	{
		if (!s_Pg[i]) continue;
		if (i == page) lv_obj_remove_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
	}
	if (set)
	{
		s_nLast = page;
		for (int i = 0; i < N_TABS; i++) if (kTabPage[i] == page) seg_set (SEG_TAB, i);
	}
	// the page's own key (UX review N1 / N2): a page reached from another lights that one (PROJECT, STATUS: HOME; the
	// machine view, SCOPE: VOICE; POOL: SET); never SET for a page that is not a setting
	int nav = page == PG_HOME || page == PG_PROJECT || page == PG_STATUS ? NAV_HOME : page == PG_SEQ ? NAV_SEQ
		: page == PG_RYTM ? NAV_PAT : page == PG_VOICE || page == PG_MACH || page == PG_SCOPE || page == PG_CHOP ? NAV_VOICE
		: page == PG_INS ? NAV_FX : page == PG_SONG ? NAV_SONG : page == PG_REC ? NAV_REC : set || page == PG_POOL ? NAV_SET : -1;
	s_nNavLit = nav;
	for (int i = 0; i < NAV_N; i++) if (s_Nav[i]) th_key_check (s_Nav[i], i == nav);
	if (!oled ())
	{
		if (set) lv_obj_remove_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);
	}
	stale_view ();
}

static void stale_view (void)		// F2: the banner on a mirror page while the Rytm is away
{
	if (!s_Stale) return;
	int on = s_bHaveSt && !s_St.rytm && (s_nPage == PG_VOICE || s_nPage == PG_INS || s_nPage == PG_RYTM);
	if (on) lv_obj_remove_flag (s_Stale, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Stale, LV_OBJ_FLAG_HIDDEN);
}

static void voice_view (void)		// the machine and drone controls for s_nVoice, from the last state given
{
	const char *n = s_St.track[s_nVoice];
	int m = -1;
	if (n)
	{
		for (int k = 0; k < N_MACH; k++)
		{
			const char *a = n, *b = kMachMap[k];
			for (; *a && *b && (*a | 0x20) == (*b | 0x20); a++, b++) {}
			if (!*a && !*b) m = k;
		}
		if (m == 4 && s_Sel[SEG_MACH] == 5) m = 5;	// DSPS and GRAIN share a machine: keep the one picked
	}
	seg_set (SEG_MACH, m);
	seg_set (SEG_DRONE, s_St.drone == -1 ? 0 : s_St.drone == s_nVoice ? 1 : s_St.drone == 8 ? 2 : -1);
	char b[40];
	if (s_St.drone == -1) lv_snprintf (b, sizeof b, "Drone");
	else if (s_St.drone == 8) lv_snprintf (b, sizeof b, "Drone: all voices");
	else lv_snprintf (b, sizeof b, "Drone: voice %d", s_St.drone + 1);
	set_text (s_DroneInfo, b);
}

static void style_async (void *p) { sidecar_ui_style ((int) (intptr_t) p); }

static void seg_cb (lv_obj_t *m, int i, void *ud)
{
	(void) m;
	int id = (int) (intptr_t) ud;
	s_Sel[id] = i;							// the control has checked it already
	char b[40];
	switch (id)
	{
	case SEG_TAB:	if (kTabPage[i] == PG_POOL) s_nPoolFrom = s_nPage;
			show_page (kTabPage[i]); break;
	case SEG_NOTES:	command (i ? "notes=1" : "notes=0"); break;
	case SEG_OUT:	command (i ? "out=tracks" : "out=main"); break;
	case SEG_MAC:	command (i ? "mac=on" : "mac=off"); break;
	case SEG_VOICE:	s_nVoice = kVoiceOrder[i & 7]; s_Sel[SEG_MACH] = -2; th_seg_set (s_Seg[SEG_MACH], -1); voice_view (); break;
	case SEG_MACH:
		lv_snprintf (b, sizeof b, "notemach=%d:%s", s_nVoice, kMachCmd[i]);
		command (b);
		break;
	case SEG_CLOCK:
		lv_snprintf (b, sizeof b, "cpumhz=%d", kClockMHz[i]);
		command (b);
		break;
	case SEG_SAVER:
		lv_snprintf (b, sizeof b, "screensaver=%d", kSaverMin[i]);
		command (b);
		break;
	case SEG_LOOK:
		sidecar_dot_look (i);
		command (i ? "uilook=stage" : "uilook=studio");
		lv_obj_invalidate (lv_screen_active ());
		break;
	case SEG_STYLE:
		command (i ? "uistyle=classic" : "uistyle=oled");
		lv_async_call (style_async, (void *) (intptr_t) i);	// rebuilt after this event (its key is deleted then)
		return;
	case SEG_DRONE:
		if (i == 0) command ("drone=off");
		else if (i == 2) command ("drone=all");
		else
		{
			lv_snprintf (b, sizeof b, "drone=%d", s_nVoice);
			command (b);
		}
		break;
	}
}

static void fifo_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int n = (int) s_nFifo + (int) (intptr_t) ud * 8;
	s_nFifo = (unsigned) (n < 24 ? 24 : n > 1024 ? 1024 : n);
	char b[24];
	lv_snprintf (b, sizeof b, "fifo=%u", s_nFifo);
	command (b);
	lv_snprintf (b, sizeof b, "%u frames", s_nFifo);
	set_text (s_FifoVal, b);
}

static void kill_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; command ("killfx"); }
static void show_page (int page);
static void ins_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; show_page (PG_INS); }	// fx-ins
static void nav_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	static const int kPage[NAV_N] = {PG_HOME, PG_SEQ, PG_RYTM, PG_VOICE, PG_INS, PG_SONG, PG_REC, -1};
	int n = (int) (intptr_t) ud;
	if (n == NAV_VOICE) command ("voicepick=-1");		// VOICE: the Rytm's selected track again
	show_page (n == NAV_SET ? s_nLast : kPage[n]);
}
static void track_cb (lv_obj_t *o, int i, void *ud)		// a voice on the home page: VOICE for its first track (UX review
{								// T3; GRANULAR / WAVETABLE: their machine view, as UP + T4 on the Rytm)
	(void) o; (void) i;
	static const int kFirstTrack[8] = {0, 4, 1, 5, 8, 6, 10, 2};	// voice -> its first Rytm track (BD BT SD LT CH MT CY RS)
	s_nVoice = (int) (intptr_t) ud;
	const char *n = s_St.track[s_nVoice];
	if (n && (!strcmp (n, "GRAN") || !strcmp (n, "WAVE")))
	{
		sidecar_wgview_want (s_nVoice);
		show_page (PG_MACH);
		return;
	}
	char b[24];
	lv_snprintf (b, sizeof b, "voicepick=%d", kFirstTrack[s_nVoice & 7]);
	command (b);
	show_page (PG_VOICE);
}

static void seq_cmd (const char *c, void *p) { (void) p; command (c); }
static void to_rytm (void) { show_page (PG_RYTM); }
static void to_seq (void) { show_page (PG_SEQ); }
static void song_cmd (const char *c) { command (c); }
static void rec_cmd (const char *c) { command (c); }
static void rec_hit_cb (lv_event_t *e) { if (lv_event_get_code (e) == LV_EVENT_CLICKED) show_page (PG_REC); }
static void rec_hit (lv_obj_t *parent, int x, int y, int w, int h)	// the home page's REC status: a tap opens REC
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (o, rec_hit_cb, LV_EVENT_CLICKED, 0);
}
static void status_hit_cb (lv_event_t *e) { if (lv_event_get_code (e) == LV_EVENT_CLICKED) show_page (PG_STATUS); }
static void status_hit (lv_obj_t *parent, int x, int y, int w, int h)	// the home page's engine numbers: a tap opens STATUS
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (o, status_hit_cb, LV_EVENT_CLICKED, 0);
}
static void project_cmd (const char *c) { command (c); }
static void project_hit_cb (lv_event_t *e) { if (lv_event_get_code (e) == LV_EVENT_CLICKED) show_page (PG_PROJECT); }
static void project_hit (lv_obj_t *parent, int x, int y, int w, int h)	// the home page's PROJECT panel: a tap opens PROJECT
{
	lv_obj_t *o = th_box (parent, x, y, w, h);
	lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (o, project_hit_cb, LV_EVENT_CLICKED, 0);
}
// the POOL page: the machine view's ALL POOLS or Settings > Pool opens it, its DONE goes back to where it came from
static void pool_page (int open)
{
	if (open) { if (s_nPage != PG_POOL) s_nPoolFrom = s_nPage; show_page (PG_POOL); }
	else show_page (s_nPoolFrom == PG_POOL ? PG_HOME : s_nPoolFrom);
}
// VOICE's own taps: its scope opens SCOPE (following the track), MULTISAMPLE's panel the instrument banks
static void voice_tap (int what)
{
	if (what == SIDECAR_VOICE_TAP_SCOPE) { sidecar_scope_debug (-1, sidecar_scope_timebase (), sidecar_scope_gain (), 0); show_page (PG_SCOPE); return; }
	sidecar_wgview_want (-1);
	show_page (PG_MACH);
	sidecar_wgview_list (1);
}

static void bright_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o; (void) ud;
	int pct = v & 0xFFFF;
	char b[24];
	sidecar_ui_dim (pct);						// live while dragging
	lv_snprintf (b, sizeof b, "%d %%", pct);
	set_text (s_BrightVal, b);
	if (v & TH_RELEASED)						// stored once, on release
	{
		lv_snprintf (b, sizeof b, "brightness=%d", pct);
		command (b);
	}
}

// ---- the OLED layout: 400x240 dots (512x300 on 1024x600), the navigation column at the right edge
// Everything is placed from the 400x240 layout plus what the screen adds (TH->dx, dy: 0, or 112 x 60 dots): panels and
// controls get wider and taller, the text keeps its size (docs/UI-THEMES.md "Sizes").

#define OW	(TH->w - 48)		// the content's width (352, 464)
#define NAVX	(TH->w - 45)
#define UH	(TH->h)
#define TBH	13			// a title bar
#define VS(v)	((v) * (UH - 41) / 199)	// a settings page's y and heights, stretched to its panel (199 or 259 dots)

static void oled_nav (lv_obj_t *scr)
{
	const int p = UH / NAV_N;		// 30 or 37 dots a key (60 / 74 px: above the 42 px touch minimum)
	for (int i = 0; i < NAV_N; i++)
	{
		s_Nav[i] = th_key (scr, NAVX, i * p, 45, (i == NAV_N - 1 ? UH - i * p : p) - 1, kNavName[i], TH_KEY_NAV, nav_cb, (void *) (intptr_t) i);
		th_key_sub (s_Nav[i], nav_t (i));		// its UP + T trig, in the key's corner
	}
}

static void oled_home (lv_obj_t *h)
{
	s_Title = th_titlebar (h, 0, 0, OW, TBH);
	th_titlebar_set (s_Title, "SIDECAR EXPANDER", "", 0);
	const int cw = (OW - 4) / 2, ew = cw - 174;		// a column: 174 or 230 dots
	const int th = 106 + TH->dy / 2, by = 124 + TH->dy / 2, bh = UH - by, ey = TH->dy / 6;

	lv_obj_t *tp = th_panel (h, 0, 15, cw, th, "VOICES", TH_PANEL_GROUP);
	const int kp = (th - 14) / 4, kw = (cw - 8) / 2;	// 23 / 30 dots a row, keys 83 / 111 wide
	for (int i = 0; i < SIDECAR_UI_TRACKS; i++)
	{
		s_Track[i] = th_key (tp, 3 + (i / 4) * (kw + 2), 12 + (i % 4) * kp, kw, kp - 2, "-", TH_KEY | TH_KEY_LEFT, track_cb, (void *) (intptr_t) kVoiceOrder[i]);
	}

	// UX review T1: NOW - what matters on stage: the pattern (L3: the page's hero value) and the transport, the
	// recorder, one health line (the core loads and Overbridge numbers are STATUS's: a tap opens it)
	lv_obj_t *ep = th_panel (h, cw + 4, 15, cw, th, "NOW", TH_PANEL_GROUP);
	(void) ew;
	s_NowPat = label (ep, 6, 14, TH->f_big, TH->ink, "-");
	s_NowTr = label_w (ep, 66, 20, cw - 72, TH->f_body, TH->dim, "-");
	label (ep, 6, 44 + ey, TH->f_micro, TH->dim, "REC");
	s_NowRec = label_w (ep, 50, 42 + ey, cw - 56, TH->f_body, TH->ink, "-");
	label (ep, 6, th - 32, TH->f_micro, TH->dim, "HEALTH");
	s_Health = label_w (ep, 50, th - 34, cw - 56, TH->f_body, TH->ink, "-");
	label_w (ep, 6, th - 16, cw - 12, TH->f_micro, TH->faint, "TAP: STATUS");
	status_hit (ep, 0, 0, cw, th);

	lv_obj_t *up = th_panel (h, 0, by, cw, bh, "PROJECT", TH_PANEL_GROUP);
	s_Project = label_w (up, 6, 13, cw - 10, TH->f_title, TH->ink, "-");
	const int sp = 16 + TH->dy / 10;
	for (int i = 0; i < 5; i++)
	{
		char n[4];
		lv_snprintf (n, sizeof n, "U%d", i + 1);
		label (up, 6, 34 + i * sp, TH->f_micro, TH->dim, n);
		s_USlot[i] = label_w (up, 24, 33 + i * sp, cw - 28, TH->f_body, TH->dim, "-");
	}
	project_hit (up, 0, 0, cw, bh);

	// the touch test (until hidden), and in its place what the display already knows
	s_TouchPanel = th_panel (h, cw + 4, by, cw, bh, "TOUCH TEST", TH_PANEL_GROUP);
	th_key (s_TouchPanel, 4, 13, 60, 27 + ey, "TAP", TH_KEY_ACCENT, tap_cb, 0);
	s_Taps = label (s_TouchPanel, 6, 44 + ey, TH->f_micro, TH->ink, "TAPS 0");
	lv_obj_t *sl = th_slider (s_TouchPanel, 72, 16 + ey / 2, 69 + ew, 21, 0, 127, slider_cb, 0);
	th_slider_set (sl, 0);
	s_SliderVal = label (s_TouchPanel, 147 + ew, 21 + ey / 2, TH->f_body, TH->ink, "0");
	label (s_TouchPanel, 72, 44 + ey, TH->f_micro, TH->dim, "RAW > SCREEN");
	s_Touch = label_w (s_TouchPanel, 6, 56 + ey, cw - 10, TH->f_micro, TH->ink, "NO TOUCH YET");
	s_Note = label_w (s_TouchPanel, 6, 68 + ey, cw - 10, TH->f_micro, TH->dim, "");
	th_key (s_TouchPanel, 4, 82 + ey, cw - 8, 29 + 2 * ey, "WORKS - HIDE", TH_KEY, hide_cb, 0);

	s_InfoPanel = th_panel (h, cw + 4, by, cw, bh, "SIDECAR", TH_PANEL_GROUP);
	static const char *const kInfoName[4] = {"NET", "WEB", "LINK", "REC"};
	const int ip = 23 + TH->dy / 8;
	for (int i = 0; i < 4; i++)
	{
		label (s_InfoPanel, 6, 19 + i * ip, TH->f_micro, TH->dim, kInfoName[i]);
		s_Info[i] = label_w (s_InfoPanel, 36, 17 + i * ip, cw - 40, TH->f_body, TH->ink, "-");
	}
	s_Rec = s_Info[3];
	rec_hit (s_InfoPanel, 0, 17 + 3 * ip - 3, cw, ip);
	lv_obj_add_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN);
	rec_hit (h, OW / 2, 0, OW / 2, 21);					// over the title bar's REC status
}

static void oled_sound (lv_obj_t *pg)
{
	const int lw = 172 + TH->dx / 2, rx = lw + 4, rw = OW - rx;	// 172 + 176, or 228 + 232
	lv_obj_t *l = th_panel (pg, 0, 41, lw, UH - 41, "ENGINE", TH_PANEL_GROUP);
	label (l, 6, VS (13), TH->f_micro, TH->dim, "MIDI NOTES");
	seg (l, SEG_NOTES, 4, VS (24), lw - 8, VS (22), kOnOff, TH_KEY);
	label (l, 6, VS (52), TH->f_micro, TH->dim, "OUTPUT");
	seg (l, SEG_OUT, 4, VS (63), lw - 8, VS (22), kOutMap, TH_KEY);
	label (l, 6, VS (91), TH->f_micro, TH->dim, "COMPUTER AUDIO (USB-C)");
	seg (l, SEG_MAC, 4, VS (102), lw - 8, VS (22), kOnOff, TH_KEY);
	label_w (l, 6, VS (138), lw - 12, TH->f_micro, TH->faint, "FIFO, DRONE, CLOCK: SYSTEM");

	lv_obj_t *r = th_panel (pg, rx, 41, rw, UH - 41, "MIDI NOTES IN", TH_PANEL_GROUP);
	seg (r, SEG_VOICE, 4, VS (13), rw - 8, VS (46), kVoiceMap2, TH_KEY_TRIG);
	label (r, 6, VS (65), TH->f_micro, TH->dim, "MIDI-ONLY VOICE MACHINE");	// (T18: not the track's machine)
	seg (r, SEG_MACH, 4, VS (76), rw - 8, VS (46), kMachMap2, TH_KEY | TH_KEY_SMALL);
	label_w (r, 6, VS (130), rw - 12, TH->f_micro, TH->faint, "PLAYS MIDI NOTES SENT TO THE");
	label_w (r, 6, VS (130) + 11, rw - 12, TH->f_micro, TH->faint, "VOICE. A TRACK'S MACHINE IS");
	label_w (r, 6, VS (130) + 22, rw - 12, TH->f_micro, TH->faint, "THE RYTM'S SRC PAGE.");
	th_seg_set (s_Seg[SEG_VOICE], kVoicePos[s_nVoice & 7]);
	s_Sel[SEG_VOICE] = kVoicePos[s_nVoice & 7];
}

static void oled_service (lv_obj_t *pg)		// UX review T18: the engineering controls, for testing
{
	lv_obj_t *p = th_panel (pg, 0, 41, OW, UH - 41, "SYSTEM", TH_PANEL_GROUP);
	const int w = OW - 12;
	label (p, 6, VS (13), TH->f_micro, TH->dim, "FIFO TARGET (THE OUTPUT'S BUFFER)");
	th_key (p, 4, VS (24), 36, VS (24), "-", TH_KEY, fifo_cb, (void *) (intptr_t) -1);
	s_FifoVal = label_w (p, 42, VS (24) + (VS (24) - 9) / 2, 120, TH->f_body, TH->ink, "96 frames");
	lv_obj_set_style_text_align (s_FifoVal, LV_TEXT_ALIGN_CENTER, 0);
	th_key (p, 164, VS (24), 36, VS (24), "+", TH_KEY, fifo_cb, (void *) (intptr_t) 1);
	s_DroneInfo = label (p, 6, VS (63), TH->f_micro, TH->dim, "DRONE");
	seg (p, SEG_DRONE, 4, VS (74), w, VS (22), kDroneMap2, TH_KEY);
	label_w (p, 6, VS (104), w, TH->f_micro, TH->faint, "A DRONE: A STEADY TEST TONE ON THE VOICE PICKED ON SOUND.");
	label_w (p, 6, VS (104) + 11, w, TH->f_micro, TH->faint, "THE WEB PAGE (SIDECAR.LOCAL) SHOWS AND SETS THE SAME.");
	label (p, 6, VS (130), TH->f_micro, TH->dim, "CPU CLOCK");
	seg (p, SEG_CLOCK, 4, VS (141), w, VS (22), kClockMap, TH_KEY);
	label_w (p, 6, VS (169), w, TH->f_micro, TH->faint, "LOWER = COOLER, LESS HEADROOM (THE VOICE BUDGET FOLLOWS).");
	label_w (p, 6, VS (169) + 11, w, TH->f_micro, TH->faint, "THE CASE FAN IS THE CASE BOARD'S: THE PI DOES NOT SET IT.");
}

static void oled_fx (lv_obj_t *pg)
{
	const int dx = TH->dx, dy = TH->dy;
	lv_obj_t *p = th_panel (pg, 0, 41, OW, UH - 41, "SEND FX: THE KIT'S, READ ONLY", TH_PANEL_GROUP);
	for (int k = 0; k < 2; k++)
		s_FxName[k] = label_w (p, 6, 14 + k * 15, 212 + dx, TH->f_body, k ? TH->ink : th_oled_secondary (127), "-");
	s_Kill = th_key (p, 266 + dx, 14, 80, 24, "KILL TAILS", TH_KEY | TH_KEY_SMALL, kill_cb, 0);	// T19: a small service key (STOP STOP does it live)
	const int tp = (OW - 16) / SIDECAR_UI_SENDS, bw = 7 + (tp - 28) / 3;	// 28 / 37 dots a track, bars 7 / 10
	for (int t = 0; t < SIDECAR_UI_SENDS; t++)
	{
		int x = 9 + t * tp;
		for (int k = 0; k < 2; k++)
		{
			s_Bar[k][t] = th_meter (p, x + k * (bw + 2), 50, bw, 111 + dy, 127, TH_METER_V, k ? TH->ink : th_oled_secondary (127));
			th_meter_set (s_Bar[k][t], 0);
			label (p, x + k * (bw + 2) + (bw - 5) / 2, 41, TH->f_micro, k ? TH->ink : th_oled_secondary (127), k ? "2" : "1");	// T19
		}
		label (p, x + bw - 5, 164 + dy, TH->f_micro, TH->dim, kSendName[t]);
	}
	th_key (p, 6, 176 + dy, 84, 20, "INSERTS", TH_KEY | TH_KEY_SMALL, ins_cb, 0);	// fx-ins: the INSERT FX page
	label_w (p, 98, 182 + dy, 248 + dx, TH->f_micro, TH->faint, "KILL TAILS: BOTH CLEARED (STOP STOP)");
}

static void oled_about (lv_obj_t *pg)
{
	lv_obj_t *p = th_panel (pg, 0, 41, OW, UH - 41, "ABOUT", TH_PANEL_GROUP);
	const int rp = 22 + TH->dy / 8;
	for (int i = 0; i < 8; i++)
	{
		label (p, 6, 18 + i * rp, TH->f_micro, TH->dim, kAboutName[i]);
		s_About[i] = label_w (p, 75, 16 + i * rp, 271 + TH->dx, TH->f_body, TH->ink, "-");
	}
}

static void oled_display (lv_obj_t *pg)
{
	const int dx = TH->dx;
	lv_obj_t *p = th_panel (pg, 0, 41, OW, UH - 41, "DISPLAY", TH_PANEL_GROUP);
	label (p, 6, VS (13) + (VS (24) - 7) / 2, TH->f_micro, TH->dim, "UI STYLE");
	seg (p, SEG_STYLE, 84, VS (13), 262 + dx, VS (24), kStyleMap, TH_KEY);
	label (p, 6, VS (43) + (VS (24) - 7) / 2, TH->f_micro, TH->dim, "SCREENSAVER");
	seg (p, SEG_SAVER, 84, VS (43), 262 + dx, VS (24), kSaverMap, TH_KEY);
	label (p, 6, VS (73) + (VS (24) - 7) / 2, TH->f_micro, TH->dim, "TOUCH TEST");	// (UX review T1: here, not on HOME)
	th_key (p, 84, VS (73), 60, VS (24), "TAP", TH_KEY, tap_cb, 0);
	s_Taps2 = label (p, 150, VS (73) + (VS (24) - 7) / 2, TH->f_micro, TH->ink, "TAPS 0");
	s_Touch2 = label_w (p, 206, VS (73) + (VS (24) - 7) / 2, 140 + dx, TH->f_micro, TH->dim, "NO TOUCH YET");
	label (p, 6, VS (106) + (VS (22) - 6) / 2, TH->f_micro, TH->dim, "BRIGHTNESS");
	s_Bright = th_slider (p, 84, VS (106), 204 + dx, VS (22), 5, 100, bright_cb, 0);
	s_BrightVal = label (p, 297 + dx, VS (106) + (VS (22) - 12) / 2, TH->f_body, TH->ink, "100 %");
	label (p, 6, VS (136) + (VS (24) - 7) / 2, TH->f_micro, TH->dim, "LOOK");			// UX review L1
	seg (p, SEG_LOOK, 84, VS (136), 262 + dx, VS (24), kLookMap, TH_KEY);
	label (p, 6, VS (166) + (VS (20) - 7) / 2, TH->f_micro, TH->dim, "TOUCH CAL");		// touch_cal.h
	th_key (p, 84, VS (166), 80, VS (20), "CALIBRATE", TH_KEY | TH_KEY_SMALL, cal_cb, 0);
	th_key (p, 170, VS (166), 50, VS (20), "RESET", TH_KEY | TH_KEY_SMALL, calreset_cb, 0);
	s_CalState = label_w (p, 228, VS (166) + (VS (20) - 7) / 2, 118 + dx, TH->f_micro, TH->ink, "DEFAULT");
	label_w (p, 6, VS (190), 340 + dx, TH->f_micro, TH->faint, "STAGE: WHOLE DOTS. STORED ON THE CARD (/.RYTM/DISPLAY.TXT).");
}

static void oled_build (lv_obj_t *scr)
{
	s_Home = th_box (scr, 0, 0, OW, UH);
	s_Pages = th_box (scr, 0, 0, OW, UH);
	lv_obj_add_flag (s_Pages, LV_OBJ_FLAG_HIDDEN);
	s_SetTitle = th_titlebar (s_Pages, 0, 0, OW, TBH);
	th_titlebar_set (s_SetTitle, "SETTINGS", "", 0);
	seg (s_Pages, SEG_TAB, 0, 16, OW, 22, kTabMap, TH_KEY);
	for (int i = 0; i < PG_N; i++)
		if (i == PG_SOUND || i == PG_FX || i == PG_ABOUT || i == PG_DISPLAY || i == PG_SERVICE)
		{
			s_Pg[i] = th_box (s_Pages, 0, 0, OW, UH);
			lv_obj_add_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
		}
	oled_sound (s_Pg[PG_SOUND]);
	oled_fx (s_Pg[PG_FX]);
	oled_about (s_Pg[PG_ABOUT]);
	oled_display (s_Pg[PG_DISPLAY]);
	oled_service (s_Pg[PG_SERVICE]);
	oled_home (s_Home);
	sidecar_seqed_create (scr, 0, seq_cmd, 0);
	sidecar_seqed_set_rytm_cb (to_rytm);
	sidecar_rytmpat_create (scr, 0, to_seq);
	sidecar_wgview_create (scr, 0);
	sidecar_wgview_set_command (command);
	sidecar_wgview_set_page (pool_page);
	sidecar_voice_create (scr, 0);
	sidecar_insfx_create (scr, 0);
	sidecar_insfx_set_command (command);
	sidecar_song_create (scr, 0, song_cmd);
	sidecar_rec_create (scr, 0, rec_cmd);
	sidecar_scope_create (scr, 0);
	sidecar_status_create (scr, 0);
	sidecar_project_create (scr, 0, project_cmd);
	sidecar_chop_create (scr, 0);
	sidecar_chop_set_command (command);
	sidecar_voice_set_tap (voice_tap);
	oled_nav (scr);
}

// ---- the CLASSIC layout: 800x480 (1024x600: the same, wider and taller by TH->dx, dy = 224 x 120 px), in the Rytm's
// hardware look

#define CY(v)	((v) * (358 + TH->dy) / 358)	// a settings page's y, stretched to its panel (358 or 478 px)
#define CH(v)	((v) + TH->dy / 15)		// a control's height there (48 or 56)

static void classic_sound (lv_obj_t *pg)
{
	const lv_font_t *sm = &lv_font_montserrat_14;
	const int dx = TH->dx / 2, ph = 358 + TH->dy;
	lv_obj_t *l = th_panel (pg, 12, 0, 380 + dx, ph, "ENGINE", TH_PANEL_GROUP);
	label (l, 12, CY (30), sm, TH->dim, "MIDI notes");
	seg (l, SEG_NOTES, 12, CY (48), 354 + dx, CH (48), kOnOff, TH_KEY);
	label (l, 12, CY (100), sm, TH->dim, "Output");
	seg (l, SEG_OUT, 12, CY (118), 354 + dx, CH (48), kOutMap, TH_KEY);
	label (l, 12, CY (170), sm, TH->dim, "Computer audio (USB-C)");
	seg (l, SEG_MAC, 12, CY (188), 354 + dx, CH (48), kOnOff, TH_KEY);
	label (l, 12, CY (252), sm, TH->sub, "FIFO, the test drone, the CPU clock: System.");

	lv_obj_t *r = th_panel (pg, 400 + dx, 0, 388 + dx, ph, "MIDI notes in", TH_PANEL_GROUP);
	label (r, 12, CY (30), sm, TH->dim, "Voice");
	seg (r, SEG_VOICE, 12, CY (48), 364 + dx, CH (48), kVoiceMap, TH_KEY_TRIG);
	th_seg_gap (s_Seg[SEG_VOICE], 2);				// eight keys of 44 px
	th_seg_font (s_Seg[SEG_VOICE], &lv_font_montserrat_12);	// "CH/OH" fits at 12
	label (r, 12, CY (100), sm, TH->dim, "MIDI-only voice machine");	// (T18: not the track's machine)
	seg (r, SEG_MACH, 12, CY (118), 364 + dx, CH (48), kMachMap, TH_KEY | TH_KEY_SMALL);
	lv_obj_t *h = label_w (r, 12, CY (180), 364 + dx, sm, TH->sub, "Plays the MIDI notes sent to the voice. A track's machine is the Rytm's SRC page.");
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
	th_seg_set (s_Seg[SEG_VOICE], kVoicePos[s_nVoice & 7]);
	s_Sel[SEG_VOICE] = kVoicePos[s_nVoice & 7];
}

static void classic_service (lv_obj_t *pg)	// UX review T18: the engineering controls, for testing
{
	const lv_font_t *sm = &lv_font_montserrat_14;
	const int dx = TH->dx, ph = 358 + TH->dy;
	lv_obj_t *l = th_panel (pg, 12, 0, 776 + dx, ph, "System", TH_PANEL_GROUP);
	label (l, 12, CY (30), sm, TH->dim, "FIFO target (the output's buffer)");
	lv_obj_t *k = th_key (l, 12, CY (48), 96, CH (48), "-", TH_KEY, fifo_cb, (void *) (intptr_t) -1);
	th_key_font (k, &lv_font_montserrat_24);
	lv_obj_t *wv = th_panel (l, 116, CY (48), 146, CH (48), "", TH_PANEL_WINDOW);
	s_FifoVal = label_w (wv, 0, 13 + (CH (48) - 48) / 2, 146, &lv_font_montserrat_20, TH->win_ink, "96 frames");
	lv_obj_set_style_text_align (s_FifoVal, LV_TEXT_ALIGN_CENTER, 0);
	k = th_key (l, 270, CY (48), 96, CH (48), "+", TH_KEY, fifo_cb, (void *) (intptr_t) 1);
	th_key_font (k, &lv_font_montserrat_24);
	s_DroneInfo = label (l, 12, CY (120), sm, TH->dim, "Drone");
	seg (l, SEG_DRONE, 12, CY (138), 364, CH (48), kDroneMap, TH_KEY);
	lv_obj_t *h = label_w (l, 12, CY (200), 750 + dx, sm, TH->sub, "A drone is a steady test tone on the voice picked on Sound. The web page (sidecar.local) shows and sets the same.");
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
	label (l, 12, CY (248), sm, TH->dim, "CPU clock");
	seg (l, SEG_CLOCK, 12, CY (266), 364, CH (48), kClockMap, TH_KEY);
	h = label_w (l, 392 + dx, CY (266), 372, sm, TH->sub, "Lower: cooler, with less headroom for voices (the guard's budget follows). The case fan is the case board's own; the Pi does not control it.");
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
}

static void classic_fx (lv_obj_t *pg)
{
	const int dx = TH->dx, dy = TH->dy;
	lv_obj_t *p = th_panel (pg, 12, 0, 776 + dx, 358 + dy, "Send FX  -  the Rytm's kit sets these, read only", TH_PANEL_GROUP);
	for (int k = 0; k < 2; k++)
		s_FxName[k] = label (p, 12 + k * (250 + dx / 4), 34, &lv_font_montserrat_24, k ? TH->ink : 0x1E7A3E, "-");
	s_Kill = th_key (p, 614 + dx, 26, 150, 52, "Kill tails", TH_KEY_DANGER, kill_cb, 0);
	th_key (p, 500 + dx, 26, 106, 52, "Inserts", TH_KEY, ins_cb, 0);	// fx-ins: the INSERT FX page
	th_key_font (s_Kill, &lv_font_montserrat_20);
	th_key_led (s_Kill, LED_RED + 15);
	lv_obj_t *w = th_panel (p, 8, 88, 760 + dx, 208 + dy, "", TH_PANEL_WINDOW);
	const int tp = 62 + dx / 12, bw = 22 + dx / 48;		// 62 / 80 px a track, bars 22 / 26
	for (int t = 0; t < SIDECAR_UI_SENDS; t++)
	{
		int x = 10 + t * tp;
		for (int k = 0; k < 2; k++)
		{
			s_Bar[k][t] = th_meter (w, x + k * (bw + 4), 12, bw, 160 + dy, 127, TH_METER_V, k ? 0xE8E8E8 : TH->accent);
			th_meter_set (s_Bar[k][t], 0);
		}
		label (w, x + bw - 16, 178 + dy, &lv_font_montserrat_16, TH->win_ink, kSendName[t]);
	}
	lv_obj_t *h = label_w (p, 12, 304 + dy, 750 + dx, &lv_font_montserrat_14, TH->sub, "Green: FX 1, white: FX 2, per track. A console override "
			       "(sendlvl=, sendfx=) lasts only until the kit's next message. Kill tails clears both FX at once (the Rytm's double STOP does the same).");
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
}

static void classic_about (lv_obj_t *pg)
{
	lv_obj_t *p = th_panel (pg, 12, 0, 776 + TH->dx, 358 + TH->dy, "About", TH_PANEL_WINDOW);
	const int rp = 36 + TH->dy / 8;
	for (int i = 0; i < 8; i++)
	{
		label (p, 16, 40 + i * rp, &lv_font_montserrat_14, TH->win_dim, kAboutName[i]);
		s_About[i] = label_w (p, 190, 38 + i * rp, 570 + TH->dx, &lv_font_montserrat_16, TH->win_ink, "-");
		lv_label_set_long_mode (s_About[i], LV_LABEL_LONG_MODE_DOTS);
	}
}

static void classic_display (lv_obj_t *pg)
{
	const lv_font_t *sm = &lv_font_montserrat_14;
	const int dx = TH->dx, d = (CH (48) - 48) / 2;	// labels stay centred on their taller controls
	lv_obj_t *p = th_panel (pg, 12, 0, 776 + dx, 358 + TH->dy, "Display", TH_PANEL_GROUP);
	label (p, 16, CY (36) + 14 + d, sm, TH->ink, "UI style");
	seg (p, SEG_STYLE, 190, CY (36), 300, CH (48), kStyleMap, TH_KEY);
	label (p, 504, CY (36) + 8 + d, sm, TH->sub, "OLED: the screen as dots.");
	label (p, 504, CY (36) + 26 + d, sm, TH->sub, "Classic: the Rytm's panel.");
	label (p, 16, CY (98) + 14 + d, sm, TH->ink, "Screensaver");
	seg (p, SEG_SAVER, 190, CY (98), 570 + dx, CH (48), kSaverMap, TH_KEY);
	label (p, 16, CY (160) + 14 + d, sm, TH->ink, "Touch test");			// (UX review T1: here, not on Home)
	lv_obj_t *tk = th_key (p, 190, CY (160), 110, CH (48), "TAP", TH_KEY_FUNC, tap_cb, 0);
	th_key_font (tk, &lv_font_montserrat_20);
	s_Taps2 = label (p, 316, CY (160) + 14 + d, sm, TH->ink, "taps: 0");
	s_Touch2 = label_w (p, 420, CY (160) + 14 + d, 340 + dx, sm, TH->sub, "no touch yet");
	label (p, 16, CY (222) + 14 + d, sm, TH->ink, "Brightness");
	label (p, 16, CY (222) + 32 + d, &lv_font_montserrat_12, TH->sub, "(dims the picture)");
	s_Bright = th_slider (p, 190, CY (222), 460 + dx, CH (48), 5, 100, bright_cb, 0);
	s_BrightVal = label (p, 672 + dx, CY (222) + 14 + d, &lv_font_montserrat_16, TH->ink, "100 %");
	label (p, 16, CY (284) + 14 + d, sm, TH->ink, "OLED look");						// UX review L1
	seg (p, SEG_LOOK, 190, CY (284), 300, CH (40), kLookMap, TH_KEY);
	label (p, 504, CY (284) + 14 + d, sm, TH->sub, "Stage: whole dots, lighter greys.");
	label (p, 16, CY (330) + 4, sm, TH->ink, "Touch");					// touch_cal.h
	th_key (p, 190, CY (330), 130, 26, "Calibrate", TH_KEY | TH_KEY_SMALL, cal_cb, 0);
	th_key (p, 330, CY (330), 80, 26, "Reset", TH_KEY | TH_KEY_SMALL, calreset_cb, 0);
	s_CalState = label_w (p, 424, CY (330) + 4, 336 + dx, sm, TH->sub, "default (the device's range)");
}

static void classic_home (lv_obj_t *h)
{
	const int dx = TH->dx, dy = TH->dy;
	const int tw = 476 + dx * 3 / 4, th = 232 + dy / 2, by = 308 + dy / 2, bh = 160 + dy / 2;
	lv_obj_t *tp = th_panel (h, 12, 64, tw, th, "Tracks", TH_PANEL_GROUP);
	const int kw = (tw - 36) / 2, kp = 48 + dy / 8;		// keys 220 / 304 wide, a row 48 / 63 px
	for (int i = 0; i < SIDECAR_UI_TRACKS; i++)
	{
		int x = 12 + (i / 4) * (kw + 10), y = 32 + (i % 4) * kp;
		s_Track[i] = th_key (tp, x, y, kw, kp - 6, "-", TH_KEY_PAD | TH_KEY_LEFT, track_cb, (void *) (intptr_t) kVoiceOrder[i]);
		th_key_font (s_Track[i], &lv_font_montserrat_16);
	}

	const int sx = 24 + tw, sw = 800 + dx - 12 - sx;		// 500 + 288, or 668 + 344
	lv_obj_t *sp = th_panel (h, sx, 64, sw, th, "Now", TH_PANEL_WINDOW);	// UX review T1 (STATUS: a tap)
	s_NowPat = label (sp, 12, 30, &lv_font_montserrat_24, TH->win_ink, "-");
	s_NowTr = label_w (sp, 110, 36, sw - 122, &lv_font_montserrat_16, TH->win_dim, "-");
	label (sp, 12, 80 + dy / 8, &lv_font_montserrat_14, TH->win_dim, "Recorder");
	s_NowRec = label_w (sp, 110, 78 + dy / 8, sw - 122, &lv_font_montserrat_16, TH->win_ink, "-");
	s_Rec = 0;
	rec_hit (sp, 0, 70 + dy / 8, sw, 36);
	label (sp, 12, 120 + dy / 4, &lv_font_montserrat_14, TH->win_dim, "Health");
	s_Health = label_w (sp, 110, 118 + dy / 4, sw - 122, &lv_font_montserrat_16, TH->win_ink, "-");
	label (sp, 12, th - 26, &lv_font_montserrat_12, TH->win_dim, "Tap: the status page");
	status_hit (sp, 0, 0, sw, 70 + dy / 8);
	status_hit (sp, 0, 108 + dy / 4, sw, th - 108 - dy / 4);

	const int lw = 380 + dx / 2, px = 24 + lw, pw = 800 + dx - 12 - px;	// touch test 380 / 492, project 384 / 496
	lv_obj_t *up = th_panel (h, px, by, pw, bh, "Project", TH_PANEL_WINDOW);
	s_Project = label_w (up, 12, 30, pw - 24, &lv_font_montserrat_16, TH->win_ink, "-");
	lv_label_set_long_mode (s_Project, LV_LABEL_LONG_MODE_DOTS);
	const int uc = (pw - 18) / 3;
	for (int i = 0; i < 5; i++)
	{
		int x = 12 + (i % 3) * uc, y = 62 + (i / 3) * (44 + dy / 4);
		char n[4];
		lv_snprintf (n, sizeof n, "U%d", i + 1);
		label (up, x, y, &lv_font_montserrat_12, TH->accent, n);
		s_USlot[i] = label_w (up, x, y + 16, uc - 8, &lv_font_montserrat_14, TH->win_dim, "-");
		lv_label_set_long_mode (s_USlot[i], LV_LABEL_LONG_MODE_DOTS);
	}
	project_hit (up, 0, 0, pw, bh);

	const int e = dy / 6, ew = dx / 2;
	s_TouchPanel = th_panel (h, 12, by, lw, bh, "Touch test", TH_PANEL_GROUP);
	lv_obj_t *k = th_key (s_TouchPanel, 12, 32, 110, 48 + e, "TAP", TH_KEY_FUNC, tap_cb, 0);
	th_key_font (k, &lv_font_montserrat_20);
	s_Taps = label (s_TouchPanel, 12, 84 + e, &lv_font_montserrat_14, TH->ink, "taps: 0");
	lv_obj_t *sl = th_slider (s_TouchPanel, 140, 30, 170 + ew, 40, 0, 127, slider_cb, 0);
	th_slider_set (sl, 0);
	s_SliderVal = label (s_TouchPanel, 322 + ew, 38, &lv_font_montserrat_20, TH->ink, "0");
	label (s_TouchPanel, 140, 66 + e / 2, &lv_font_montserrat_12, TH->sub, "raw -> screen");
	s_Touch = label (s_TouchPanel, 140, 82 + e / 2, &lv_font_montserrat_14, TH->ink, "no touch yet");
	s_Note = label_w (s_TouchPanel, 150, 8, 218 + ew, &lv_font_montserrat_12, TH->dim, "");
	lv_obj_set_style_text_align (s_Note, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode (s_Note, LV_LABEL_LONG_MODE_DOTS);
	k = th_key (s_TouchPanel, 12, 104 + 2 * e, lw - 26, 48 + e, "Touch works - hide", TH_KEY, hide_cb, 0);
	th_key_font (k, &lv_font_montserrat_20);

	s_InfoPanel = th_panel (h, 12, by, lw, bh, "Sidecar", TH_PANEL_WINDOW);
	static const char *const kInfoName[4] = {"Network", "Web page", "Link", "Recorder"};
	const int ip = 28 + dy / 8;
	for (int i = 0; i < 4; i++)
	{
		label (s_InfoPanel, 12, 36 + i * ip, &lv_font_montserrat_14, TH->win_dim, kInfoName[i]);
		s_Info[i] = label_w (s_InfoPanel, 110, 34 + i * ip, lw - 122, &lv_font_montserrat_16, TH->win_ink, "-");
		lv_label_set_long_mode (s_Info[i], LV_LABEL_LONG_MODE_DOTS);
	}
	rec_hit (s_InfoPanel, 0, 34 + 3 * ip - 8, lw, ip);			// the Recorder row
	lv_obj_add_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN);
}

static void classic_build (lv_obj_t *scr)
{
	const int W = TH->w;
	s_Home = th_box (scr, 0, 0, W, TH->h);
	s_Pages = th_box (scr, 0, 114, W, TH->h - 114);		// the settings pages below their tab strip
	lv_obj_add_flag (s_Pages, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < PG_N; i++)
		if (i == PG_SOUND || i == PG_FX || i == PG_ABOUT || i == PG_DISPLAY || i == PG_SERVICE)
		{
			s_Pg[i] = th_box (s_Pages, 0, 0, W, TH->h - 114);
			lv_obj_add_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
		}

	// the title bar: the header strip of the emulator's window, the wordmark, the Rytm's state, Seq and Settings keys
	lv_obj_t *bar = th_titlebar (scr, 0, 0, W, 52);
	th_titlebar_set (bar, "", "", 0);
	static const lv_image_dsc_t kWord = {
		.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = WORDMARK_A8_W, .h = WORDMARK_A8_H,
			   .stride = WORDMARK_A8_W},
		.data_size = sizeof WordmarkA8, .data = WordmarkA8};
	lv_obj_t *word = lv_image_create (bar);
	lv_image_set_src (word, &kWord);
	lv_obj_set_pos (word, 16, 11);
	lv_obj_set_style_image_recolor (word, lv_color_hex (TH->title_ink), 0);
	lv_obj_set_style_image_recolor_opa (word, LV_OPA_COVER, 0);
	lv_obj_t *acc = th_box (bar, 17, 11 + WORDMARK_A8_H + 3, 22, 3);
	lv_obj_set_style_bg_color (acc, lv_color_hex (TH->accent), 0);
	lv_obj_set_style_bg_opa (acc, LV_OPA_COVER, 0);
	lv_obj_t *rule = th_box (bar, 16 + WORDMARK_A8_W + 10, 15, 1, 20);
	lv_obj_set_style_bg_color (rule, lv_color_hex (0x44464A), 0);
	lv_obj_set_style_bg_opa (rule, LV_OPA_COVER, 0);
	lv_obj_t *sub = label (bar, 16 + WORDMARK_A8_W + 21, 18, &lv_font_montserrat_12, TH->win_dim, "EXPANDER");
	lv_obj_set_style_text_letter_space (sub, 4, 0);
	s_Rytm = label_w (bar, 16 + WORDMARK_A8_W + 21, 33, 120, &lv_font_montserrat_12, TH->win_dim, "");	// under EXPANDER
	// the eight navigation keys (UX review N1): 57 px wide at 800, 86 at 1024; the name and its UP + T trig
	const int nx0 = 300, nkw = (W - 12 - nx0 + 4) / NAV_N;
	for (int i = 0; i < NAV_N; i++)
	{
		s_Nav[i] = th_key (bar, nx0 + i * nkw, 3, nkw - 4, 44, kNavNameC[i], i == NAV_SET ? TH_KEY_FUNC : TH_KEY, nav_cb, (void *) (intptr_t) i);
		th_key_font (s_Nav[i], &lv_font_montserrat_16);
		th_key_sub (s_Nav[i], nav_t (i));
	}

	sidecar_seqed_create (scr, 52, seq_cmd, 0);
	sidecar_seqed_set_rytm_cb (to_rytm);
	sidecar_rytmpat_create (scr, 52, to_seq);
	sidecar_wgview_create (scr, 52);
	sidecar_wgview_set_command (command);
	sidecar_wgview_set_page (pool_page);
	sidecar_voice_create (scr, 52);
	sidecar_insfx_create (scr, 52);
	sidecar_insfx_set_command (command);
	sidecar_song_create (scr, 52, song_cmd);
	sidecar_rec_create (scr, 52, rec_cmd);
	sidecar_scope_create (scr, 52);
	sidecar_status_create (scr, 52);
	sidecar_project_create (scr, 52, project_cmd);
	sidecar_chop_create (scr, 52);
	sidecar_chop_set_command (command);
	sidecar_voice_set_tap (voice_tap);

	seg (scr, SEG_TAB, 12, 60, W - 24, 48, kTabMap, TH_KEY);
	th_seg_font (s_Seg[SEG_TAB], &lv_font_montserrat_16);
	lv_obj_add_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);
	classic_sound (s_Pg[PG_SOUND]);
	classic_fx (s_Pg[PG_FX]);
	classic_about (s_Pg[PG_ABOUT]);
	classic_display (s_Pg[PG_DISPLAY]);
	classic_service (s_Pg[PG_SERVICE]);
	classic_home (s_Home);
}

// ---- the link animation: a banner over the top of the screen when the Rytm's link comes up or drops (not on a page
// change or a rebuild: only a change of s->rytm). The Rytm's own CONNECTED splash is the model (core/ui/remote.cpp:
// a 150 ms ease-out slide, a short hold): the banner opens as a curtain from the left (CONN_IN), the word slides in
// from the right and a row of dots / LEDs sweeps across (up: lit left to right, down: put out right to left), then the
// curtain leaves to the right (CONN_OUT), uncovering the normal indicator last. One object on the top layer, never
// clickable, redrawn at 25 fps while it runs (its own small area, no page redraw); hidden otherwise.
enum { CONN_TICK = 40, CONN_IN = 260, CONN_OUT = 280, CONN_UP_MS = 1400, CONN_DOWN_MS = 1100 };
static lv_obj_t *s_Conn;
static lv_timer_t *s_ConnTimer;
static int s_nLink = -1;				// the link as last seen (-1: no status yet); survives a rebuild
static int s_ConnUp, s_ConnDur;
static uint32_t s_ConnT0;

static int conn_h (void) { return oled () ? 26 : 52; }
static int conn_w (void) { return oled () ? TH->w - 48 : TH->w; }
static int ease_out (int t, int T) { if (t <= 0) return 0; if (t >= T) return 256; int r = T - t; return 256 - r * r * 256 / (T * T); }
static int ease_in (int t, int T) { if (t <= 0) return 0; if (t >= T) return 256; return t * t * 256 / (T * T); }

static void conn_draw_body (lv_event_t *e);
static void conn_draw (lv_event_t *e)		// (the word slides in past its edge on purpose: not a text-fit failure)
{
	th_fit_fn keep = th_fit_report;
	th_fit_report = 0;
	conn_draw_body (e);
	th_fit_report = keep;
}
static void conn_draw_body (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	const int w = lv_area_get_width (&c), h = lv_area_get_height (&c), t = (int) (lv_tick_get () - s_ConnT0), up = s_ConnUp;
	const int vx1 = t < CONN_IN ? w * ease_out (t, CONN_IN) / 256 : w;			// the curtain's visible part [vx0, vx1)
	const int vx0 = t > s_ConnDur - CONN_OUT ? w * ease_in (t - (s_ConnDur - CONN_OUT), CONN_OUT) / 256 : 0;
	if (vx1 <= vx0) return;
	lv_area_t keep = l->_clip_area, vis = {c.x1 + vx0, c.y1, c.x1 + vx1 - 1, c.y2};
	if (!lv_area_intersect (&l->_clip_area, &keep, &vis)) { l->_clip_area = keep; return; }
	const int od = oled ();
	const char *word = od ? (up ? "CONNECTED" : "DISCONNECTED") : (up ? "RYTM CONNECTED" : "RYTM DISCONNECTED");
	const lv_font_t *f = od ? TH->f_big : TH->f_title;
	const int tw = th_text_width (f, word);
	int tx = od ? (w - tw) / 2 : w - 24 - tw;
	tx += (w - tx) * (256 - ease_out (t - 100, 380)) / 256;				// in from the right edge, easing out
	if (od)
	{
		const uint32_t hi = up ? TH->accent_hi : TH->ink, lo = up ? TH->accent : TH->dim;
		th_fill (l, &c, 0, 0, w - 1, h - 1, TH->bg, LV_OPA_COVER, 0);
		th_fill (l, &c, 0, h - 1, w - 1, h - 1, DOT_DOTTED, LV_OPA_COVER, 0);
		th_text (l, &c, tx, 1, tw + 4, f, th_mix (hi, lo, t < 400 ? 0 : t > 700 ? 256 : (t - 400) * 256 / 300), LV_TEXT_ALIGN_LEFT, word);
		// the sweep: 2x2 dots every 4 dots along the bottom, a bright head with a trail
		const int n = (w - 8) / 4, p = n * ease_out (t - 120, 760) / 256 - 1;
		for (int i = 0; i < n; i++)
		{
			uint32_t col;
			if (up) col = i > p ? 0x1E1E1E : th_mix (hi, lo, p - i < 6 ? (p - i) * 42 : 256);
			else col = n - 1 - i > p ? TH->dim : 0x1E1E1E;
			th_fill (l, &c, 4 + 4 * i, h - 5, 5 + 4 * i, h - 4, col, LV_OPA_COVER, 0);
		}
		if (t < CONN_IN) th_fill (l, &c, vx1 - 2, 0, vx1 - 1, h - 1, hi, LV_OPA_COVER, 0);
		else if (vx0 > 0) th_fill (l, &c, vx0, 0, vx0 + 1, h - 1, TH->dim, LV_OPA_COVER, 0);
	}
	else
	{
		const uint32_t lit = th_led (up ? LED_OK : LED_REC);
		const int n = 16, step = 22, x0 = 22, p = n * ease_out (t - 120, 760) / 256 - 1, cy = h / 2 - 1;
		th_fill (l, &c, 0, 0, w - 1, h - 1, TH->win, LV_OPA_COVER, 0);
		th_fill (l, &c, 0, h - 3, w - 1, h - 1, TH->win_edge, LV_OPA_COVER, 0);
		for (int i = 0; i < n; i++)
		{
			const int cx = x0 + i * step, on = up ? i <= p : n - 1 - i > p;
			if (on && up && p < n - 1 && p - i < 3)			// the glow around the head
				th_fill (l, &c, cx - 9, cy - 9, cx + 9, cy + 9, th_mix (TH->win, lit, 150 - (p - i) * 45), LV_OPA_COVER, LV_RADIUS_CIRCLE);
			th_fill (l, &c, cx - 5, cy - 5, cx + 5, cy + 5, on ? lit : th_mix (TH->win, 0x6E7176, 90), LV_OPA_COVER, LV_RADIUS_CIRCLE);
		}
		th_text (l, &c, tx, (h - 3 - f->line_height) / 2, tw + 8, f, TH->win_ink, LV_TEXT_ALIGN_LEFT, word);
		if (t < CONN_IN) th_fill (l, &c, vx1 - 3, 0, vx1 - 1, h - 1, lit, LV_OPA_COVER, 0);
		else if (vx0 > 0) th_fill (l, &c, vx0, 0, vx0 + 2, h - 1, TH->win_dim, LV_OPA_COVER, 0);
	}
	l->_clip_area = keep;
}

static void conn_tick (lv_timer_t *tm)
{
	if (!s_Conn || (int) (lv_tick_get () - s_ConnT0) >= s_ConnDur)
	{
		if (s_Conn) lv_obj_add_flag (s_Conn, LV_OBJ_FLAG_HIDDEN);
		lv_timer_delete (tm);
		s_ConnTimer = 0;
		return;
	}
	lv_obj_invalidate (s_Conn);
}

static void conn_start (int up)
{
	if (!s_Conn || !s_nBuilt) return;
	s_ConnUp = up;
	s_ConnDur = up ? CONN_UP_MS : CONN_DOWN_MS;
	s_ConnT0 = lv_tick_get ();
	lv_obj_remove_flag (s_Conn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_invalidate (s_Conn);
	if (!s_ConnTimer) s_ConnTimer = lv_timer_create (conn_tick, CONN_TICK, 0);
}

static void conn_build (lv_obj_t *top)
{
	if (s_ConnTimer) lv_timer_delete (s_ConnTimer);		// (a rebuild: the old object went with the top layer)
	s_ConnTimer = 0;
	s_Conn = th_box (top, 0, 0, conn_w (), conn_h ());
	lv_obj_remove_flag (s_Conn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag (s_Conn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb (s_Conn, conn_draw, LV_EVENT_DRAW_MAIN, 0);
}

// ---- building, the theme switch

static void dim_layer (void);

static void build (void)
{
	lv_obj_t *scr = lv_screen_active ();
	memset (s_Seg, 0, sizeof s_Seg);
	memset (s_Pg, 0, sizeof s_Pg);
	memset (s_Nav, 0, sizeof s_Nav);
	s_Title = s_Rytm = s_Rec = s_SetTitle = s_Note = s_Taps2 = s_Touch2 = s_Map = s_Stale = 0;
	s_CalOv = s_CalState = 0;					// (the top layer was cleaned)
	s_nCalStep = -1;
	s_nCalCal = -1;
	s_Blocks = s_Underruns = s_NowPat = s_NowTr = s_NowRec = s_Health = 0;
	memset (s_Load, 0, sizeof s_Load);
	memset (s_USlot, 0, sizeof s_USlot);
	if (s_UTimer) { lv_timer_delete (s_UTimer); s_UTimer = 0; }
	lv_obj_set_style_bg_color (scr, lv_color_hex (TH->bg), 0);
	lv_obj_set_style_bg_opa (scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color (scr, lv_color_hex (TH->ink), 0);
	lv_obj_set_style_text_font (scr, TH->f_body, 0);
	lv_obj_remove_flag (scr, LV_OBJ_FLAG_SCROLLABLE);
	if (oled ()) oled_build (scr); else classic_build (scr);
	if (!oled ()) lv_obj_add_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);

	// the dot under the finger (top layer, never clickable), the power warning's banner, the dimming layer
	lv_obj_t *top = lv_layer_top ();
	s_Dot = th_box (top, 0, 0, oled () ? 11 : 16, oled () ? 11 : 16);
	lv_obj_set_style_radius (s_Dot, oled () ? 0 : LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_color (s_Dot, lv_color_hex (oled () ? TH->ink : TH->accent), 0);
	lv_obj_set_style_border_width (s_Dot, oled () ? 1 : 2, 0);
	lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
	s_Power = th_titlebar (top, 0, 0, oled () ? OW : TH->w, oled () ? TBH : 52);
	lv_obj_add_flag (s_Power, LV_OBJ_FLAG_HIDDEN);
	conn_build (top);					// under the dimming layer
	// F2: VOICE, INSERT FX and PAT mirror the Rytm: while it is not connected they say they show the last they had
	s_Stale = th_label (top, oled () ? OW - 172 : TH->w - 330, oled () ? 1 : 56, oled () ? TH->f_body : &lv_font_montserrat_16, TH->bad,
			    oled () ? "NO RYTM: SHOWING LAST" : "Rytm not connected: showing the last");
	th_label_color (s_Stale, TH->bad);
	lv_obj_add_flag (s_Stale, LV_OBJ_FLAG_HIDDEN);
	{	// N3: the UP map (every UP + key: the device's table), shown while UP is held alone on the Rytm
		static char c1[400], c2[400];
		const lv_font_t *f = oled () ? TH->f_body : &lv_font_montserrat_20;
		uint32_t ink = oled () ? TH->ink : TH->win_ink;
		map_cols (!oled (), c1, c2, sizeof c1);
		s_Map = th_label (top, oled () ? 60 : 150, oled () ? 30 : 60, f, ink, c1);
		lv_point_t w1, w2, wt;
		lv_text_get_size (&w1, strchr (c1, '\n') + 1, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);	// the left column
		lv_text_get_size (&wt, c1, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		lv_text_get_size (&w2, c2, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		int x2 = w1.x + (oled () ? 14 : 28);
		th_label (s_Map, x2, 0, f, ink, c2);
		lv_obj_set_width (s_Map, (x2 + w2.x > wt.x ? x2 + w2.x : wt.x) + 2 * (oled () ? 8 : 18) + 4);
		lv_obj_set_style_bg_color (s_Map, lv_color_hex (oled () ? 0x000000 : TH->win), 0);
		lv_obj_set_style_bg_opa (s_Map, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Map, lv_color_hex (oled () ? TH->ink : TH->accent), 0);
		lv_obj_set_style_border_width (s_Map, oled () ? 1 : 2, 0);
		lv_obj_set_style_pad_all (s_Map, oled () ? 8 : 18, 0);
		if (!s_nMap) lv_obj_add_flag (s_Map, LV_OBJ_FLAG_HIDDEN);
	}
	s_Dim = 0;
	dim_layer ();
	s_nBuilt = 1;
	s_nRytm = -1;
	s_nTouchSeen = -1;
	for (int i = 0; i < SEG_N; i++) if (s_Seg[i]) s_Sel[i] = -2;
	if (s_bHaveSt)
	{
		sidecar_ui_status_t st = s_St;
		sidecar_ui_status (&st);
	}
	char b[24];
	lv_snprintf (b, sizeof b, oled () ? "TAPS %u" : "taps: %u", s_nTaps);
	set_text (s_Taps, b);
	set_text (s_Taps2, b);
	touch_view (s_nTouchShown);
	seg_set (SEG_STYLE, s_nStyle);
	seg_set (SEG_LOOK, sidecar_dot_get_look ());
	if (s_Seg[SEG_VOICE]) { th_seg_set (s_Seg[SEG_VOICE], kVoicePos[s_nVoice & 7]); s_Sel[SEG_VOICE] = kVoicePos[s_nVoice & 7]; }
	show_page (s_nPage);
}

void sidecar_ui_create (void)
{
	lv_display_t *d = lv_display_get_default ();
	if (!s_PhysW)
	{
		s_PhysW = (int) lv_display_get_horizontal_resolution (d);
		s_PhysH = (int) lv_display_get_vertical_resolution (d);
	}
	sidecar_theme_screen (s_PhysW, s_PhysH);		// 1024x600: the wide layouts
	sidecar_theme_set (s_nStyle);
	int uw = s_PhysW / TH->dot, uh = s_PhysH / TH->dot;	// OLED: dots (TH->dot physical pixels each)
	lv_display_set_resolution (d, uw < TH->w ? uw : TH->w, uh < TH->h ? uh : TH->h);	// never past a smaller screen
	build ();
}

// the redraw after a switch, in bands: one band each lv_timer_handler call (each core 0 Update draws about a quarter or a
// sixth of the screen, never all of it at once), a wipe from the top
static int s_nWipe, s_nWipeBands;
static void wipe_cb (lv_timer_t *t)
{
	lv_display_t *d = lv_display_get_default ();
	int h = (int) lv_display_get_vertical_resolution (d), w = (int) lv_display_get_horizontal_resolution (d);
	lv_area_t a = {0, s_nWipe * h / s_nWipeBands, w - 1, (s_nWipe + 1) * h / s_nWipeBands - 1};
	lv_obj_invalidate_area (lv_screen_active (), &a);
	if (++s_nWipe >= s_nWipeBands) lv_timer_delete (t);
}

void sidecar_ui_style (int style)
{
	style = style == THEME_CLASSIC ? THEME_CLASSIC : THEME_OLED;
	if (style == s_nStyle && s_nBuilt) return;
	s_nStyle = style;
	if (!s_nBuilt) return;					// sidecar_ui_create builds it
	// the old objects go first (seqed / rytmpat keep their models: their create re-reads them); nothing is drawn
	// while the pages are rebuilt, then the screen is redrawn band by band (wipe_cb)
	lv_display_t *d = lv_display_get_default ();
	lv_display_enable_invalidation (d, false);
	lv_obj_clean (lv_layer_top ());
	lv_obj_clean (lv_screen_active ());
	sidecar_ui_create ();
	lv_obj_update_layout (lv_screen_active ());		// the new objects' places, still without invalidating
	lv_obj_update_layout (lv_layer_top ());
	lv_display_enable_invalidation (d, true);
	s_nWipe = 0;
	s_nWipeBands = oled () ? 4 : 6;
	lv_timer_create (wipe_cb, 0, 0);
}

int sidecar_ui_get_style (void) { return s_nStyle; }
int sidecar_ui_dot (void) { return TH->dot; }

void sidecar_ui_origin (int *x, int *y)
{
	int x0 = (s_PhysW - TH->w * TH->dot) / 2, y0 = (s_PhysH - TH->h * TH->dot) / 2;	// 0 on 800x480 and 1024x600
	*x = x0 > 0 ? x0 : 0;
	*y = y0 > 0 ? y0 : 0;
}

void sidecar_ui_to_ui (int *x, int *y)
{
	int ox, oy, d = TH->dot;
	sidecar_ui_origin (&ox, &oy);
	lv_display_t *dd = lv_display_get_default ();
	int w = (int) lv_display_get_horizontal_resolution (dd), h = (int) lv_display_get_vertical_resolution (dd);
	int ux = (*x - ox) / d, uy = (*y - oy) / d;
	*x = ux < 0 ? 0 : ux >= w ? w - 1 : ux;
	*y = uy < 0 ? 0 : uy >= h ? h - 1 : uy;
}

int sidecar_ui_bright (void)
{
	if (!oled ()) return 256;
	return 256 - (100 - s_nDim) * 256 * 70 / (95 * 100);	// as the black layer: 5 % still about 30 % bright
}

static void dim_layer (void)
{
	if (oled ())						// the dot expansion dims (sidecar_ui_bright): a redraw
	{
		lv_obj_invalidate (lv_screen_active ());
		return;
	}
	if (!s_Dim)
	{
		s_Dim = lv_obj_create (lv_layer_top ());		// over everything, never takes a touch
		lv_obj_remove_style_all (s_Dim);
		lv_obj_set_size (s_Dim, LV_PCT (100), LV_PCT (100));
		lv_obj_set_style_bg_color (s_Dim, lv_color_hex (0x000000), 0);
		lv_obj_remove_flag (s_Dim, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_flag (s_Dim, LV_OBJ_FLAG_IGNORE_LAYOUT);
	}
	// the darkest setting (5 %) still leaves the picture readable: the black layer is at most 70 % opaque, so the
	// brightness slider can always be found again
	lv_obj_set_style_bg_opa (s_Dim, (lv_opa_t) ((100 - s_nDim) * 255 * 70 / (95 * 100)), 0);
	if (s_nDim >= 100) lv_obj_add_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_ui_dim (int percent)
{
	if (percent < 5) percent = 5;
	if (percent > 100) percent = 100;
	if (percent == s_nDim) return;
	s_nDim = percent;
	if (s_nBuilt) dim_layer ();
}

// ---- the state

static void fmt_rec (char *b, int cap, const sidecar_ui_status_t *s, const char *off)
{
	if (s->rec == 2) lv_snprintf (b, cap, "REC %u:%02u:%02u", s->rec_secs / 3600, s->rec_secs / 60 % 60, s->rec_secs % 60);
	else lv_snprintf (b, cap, "%s", s->rec == 1 ? (oled () ? "ARMED" : "REC armed") : off);
}

// HOME's U1..U5 as the last status gave them: the text, the state (rmp_slot_state: 0 empty 1 ready 2..5 a problem
// 6 loading) and when an error began; a timer moves the loading dots and blinks a new error (L2: 1 Hz for 3 s)
static void uslot_tick (lv_timer_t *t);
static void uslot_paint (void)
{
	int moving = 0;
	uint32_t now = lv_tick_get ();
	for (int i = 0; i < 5; i++)
	{
		if (!s_USlot[i]) continue;
		int st = s_UState[i], err = st >= 2 && st <= 5;
		char b[32];
		uint32_t ink = oled () ? TH->ink : TH->win_ink, dim = oled () ? TH->dim : TH->win_dim;
		if (st == 6)
		{
			static const char *const kDots[4] = {"", ".", "..", "..."};
			lv_snprintf (b, sizeof b, "%s%s", s_UText[i], kDots[now / 300 % 4]);
			moving = 1;
		}
		else lv_snprintf (b, sizeof b, "%s", s_UText[i]);
		set_text (s_USlot[i], b);
		int on = err && (now - s_UErrT0[i] >= 3000 || now / 500 % 2 == 0);	// blinking at first, then steady
		if (err && now - s_UErrT0[i] < 3000) moving = 1;
		if (oled ())
		{
			th_label_color (s_USlot[i], on ? 0x000000 : err ? TH->ink : st == 1 ? ink : dim);
			lv_obj_set_style_bg_color (s_USlot[i], lv_color_hex (TH->accent_hi), 0);
			lv_obj_set_style_bg_opa (s_USlot[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		}
		else th_label_color (s_USlot[i], err ? TH->bad : st == 1 ? ink : dim);	// CLASSIC: the hardware's red LED colour
	}
	if (moving && !s_UTimer) s_UTimer = lv_timer_create (uslot_tick, 150, 0);
	else if (!moving && s_UTimer) { lv_timer_delete (s_UTimer); s_UTimer = 0; }
}
static void uslot_tick (lv_timer_t *t) { (void) t; uslot_paint (); }

void sidecar_ui_status (const sidecar_ui_status_t *s)
{
	char b[80];
	if (s->ui_style >= 0 && s->ui_style != s_nStyleSeen)		// a change of the stored setting, not our own tap
	{
		s_nStyleSeen = s->ui_style;
		if (s->ui_style != s_nStyle)
		{
			s_St = *s;
			s_bHaveSt = 1;
			sidecar_ui_style (s->ui_style);			// its build applies s
			return;
		}
	}
	if (s->ui_look >= 0 && s->ui_look != sidecar_dot_get_look ())		// the stored look (L1)
	{
		sidecar_dot_look (s->ui_look);
		seg_set (SEG_LOOK, s->ui_look);
		if (oled ()) lv_obj_invalidate (lv_screen_active ());
	}
	int power = s->note && strncmp (s->note, "POWER", 5) == 0;	// the supply warning (power_state.h)
	if (oled ())
	{
		{
			fmt_rec (b, sizeof b, s, "");
			char r[48];
			lv_snprintf (r, sizeof r, "%s%s%s", b, b[0] ? "  " : "", s->rytm ? "RYTM" : "NO RYTM");
			th_titlebar_set (s_Title, "SIDECAR EXPANDER", r, 0);
			// REC: a white LED blinking with the take's seconds (OLED has no red); else the Rytm: teal, or none
			int blink = s->rec == 2 && (s->rec_secs & 1) == 0;
			th_titlebar_led (s_Title, s->rec == 2 ? (blink ? LED_PLAY : 0) : s->rytm ? LED_OK : 0);
			th_key_led (s_Nav[NAV_REC], s->rec == 2 && blink ? LED_PLAY : 0);
		}
	}
	else if (s->rytm != s_nRytm)
	{
		set_text (s_Rytm, s->rytm ? "Rytm connected" : "No Rytm link");
		th_label_color (s_Rytm, s->rytm ? TH->accent : 0xFF6B6B);
	}
	s_nRytm = s->rytm;
	if (s_nLink >= 0 && (s->rytm != 0) != s_nLink) conn_start (s->rytm != 0);	// a transition, not the first status
	s_nLink = s->rytm != 0;
	if (power)
	{
		lv_snprintf (b, sizeof b, "%s", s->note);
		th_titlebar_set (s_Power, b, "", 1);
		lv_snprintf (s_Banner, sizeof s_Banner, "%s", b);
		lv_obj_remove_flag (s_Power, LV_OBJ_FLAG_HIDDEN);
	}
	else if (s->temp_warn)		// the temperature warning (pi/engine/thermal.h): the same banner; the supply's warning wins
	{
		int low = s->cpu_mhz > 0 && s->cpu_mhz <= 1000;		// the clock is as low as it goes: the case fan is the other lever
		if (s->clock_low_mhz > 0)	// Circle's throttle already lowered the clock (socmaxtemp 78): say so
		{
			if (oled ()) lv_snprintf (b, sizeof b, "SOC %dC - CLOCK LOWERED TO %d MHZ", s->temp_c, s->clock_low_mhz);
			else lv_snprintf (b, sizeof b, "SoC %d \xC2\xB0" "C - clock lowered to %d MHz.", s->temp_c, s->clock_low_mhz);
		}
		else if (oled ()) lv_snprintf (b, sizeof b, low ? "SOC %dC TOO HOT: CHECK THE CASE FAN" : "SOC %dC TOO HOT: SET > SYSTEM > CLOCK", s->temp_c);
		else lv_snprintf (b, sizeof b, low ? "SoC %d \xC2\xB0" "C - too hot. Check the case fan." : "SoC %d \xC2\xB0" "C - too hot. Set > System > clock.", s->temp_c);
		th_titlebar_set (s_Power, b, "", 1);
		lv_snprintf (s_Banner, sizeof s_Banner, "%s", b);
		lv_obj_remove_flag (s_Power, LV_OBJ_FLAG_HIDDEN);
	}
	else lv_obj_add_flag (s_Power, LV_OBJ_FLAG_HIDDEN);
	{	// HOME's NOW panel (UX review T1): the pattern, the transport, the recorder, one health line (an alert inverted)
		uint32_t ink = oled () ? TH->ink : TH->win_ink, dim = oled () ? TH->dim : TH->win_dim;
		if (s->pattern >= 0) lv_snprintf (b, sizeof b, "%c%02d", 'A' + s->pattern / 16 % 8, s->pattern % 16 + 1);
		else lv_snprintf (b, sizeof b, "-");
		set_text (s_NowPat, b);
		set_text (s_NowTr, !s->rytm ? (oled () ? "NO RYTM" : "no Rytm") : s->transport == 1 ? (oled () ? "PLAYING" : "playing")
				    : s->transport == 2 ? (oled () ? "PAUSED" : "paused") : (oled () ? "STOPPED" : "stopped"));
		if (s_NowTr) th_label_color (s_NowTr, s->transport == 1 ? (oled () ? 0xFFFFFF : TH->ok) : dim);
		fmt_rec (b, sizeof b, s, oled () ? "OFF" : "off");
		set_text (s_NowRec, b);
		if (s_NowRec) th_label_color (s_NowRec, s->rec == 2 ? TH->bad : s->rec == 1 ? ink : dim);
		int hi = -1, hc = -1;
		for (int c = 0; c < SIDECAR_UI_CORES; c++) if (s->load[c] > hi) hi = s->load[c], hc = c;
		if (s->underruns) lv_snprintf (b, sizeof b, oled () ? "UNDERRUNS %u" : "%u underruns", s->underruns);
		else if (hi >= 85) lv_snprintf (b, sizeof b, oled () ? "CORE %d AT %d%%" : "core %d at %d %%", hc, hi);
		else lv_snprintf (b, sizeof b, oled () ? "ALL FINE" : "all fine");
		set_text (s_Health, b);
		if (s_Health) th_label_color (s_Health, s->underruns || hi >= 85 ? TH->bad : ink);
	}
	lv_snprintf (b, sizeof b, "%u", s->blocks);
	set_text (s_Blocks, b);
	lv_snprintf (b, sizeof b, "%u", s->underruns);
	set_text (s_Underruns, b);
	if (s_Underruns) th_label_color (s_Underruns, s->underruns ? TH->bad : oled () ? TH->ink : TH->win_ink);
	if (!oled () && s_Rec)
	{
		if (s->rec == 2 || s->rec == 1) fmt_rec (b, sizeof b, s, ""); else b[0] = 0;
		set_text (s_Rec, b);
		th_label_color (s_Rec, s->rec == 2 ? TH->bad : TH->win_dim);
	}
	for (int c = 0; c < SIDECAR_UI_CORES; c++)
	{
		if (!s_Load[c]) continue;
		int l = s->load[c];
		if (l < 0) lv_snprintf (b, sizeof b, oled () ? "C%d -" : "core %d  -", c);
		else lv_snprintf (b, sizeof b, oled () ? "C%d %d" : "core %d  %d%%", c, l);
		th_meter_label (s_Load[c], b);
		th_meter_set (s_Load[c], l < 0 ? -1 : l > 100 ? 100 : l);
	}
	for (int i = 0; i < SIDECAR_UI_TRACKS; i++)
	{
		int vi = kVoiceOrder[i];
		const char *n = s->track[vi] && s->track[vi][0] ? s->track[vi] : "-";
		if (oled ()) lv_snprintf (b, sizeof b, "%s %s", kVoiceName[vi], n);
		else lv_snprintf (b, sizeof b, "%s   %s", kVoiceName[vi], n);
		th_key_text (s_Track[i], b);
		th_key_sub (s_Track[i], s->poly >> vi & 1 ? (oled () ? "POLY TRACK" : "poly track") : "");	// S1
	}
	set_text (s_Note, s->note && !power ? s->note : "");		// (the supply warning: in the title bar)
	if (s_Note) th_label_color (s_Note, TH->dim);
	set_text (s_Project, s->project[0] ? s->project : "-");
	for (int i = 0; i < 5; i++)
	{
		// UX review T2 / L2: an error is inverted with its reason spelled out ("!" no longer a glyph that reads as I),
		// blinking 3 s then steady; loading is dim with a moving ellipsis (uslot_paint)
		int st = s->uslot_state[i];
		const char *t = s->uslot[i][0] ? s->uslot[i] : "-";
		char b[24];
		if (st >= 2 && st <= 5)
		{
			// the core's "!TAG NAME" (rmp_why_name: the Rytm's 4-letter tag, 4 of the name) with the tag spelled
			// out; just the word when the name does not fit too
			static const char *const kWhy[6] = {"", "", "MISSING", "BAD", "NO ROOM", "MUTED"};
			static const char *const kTag[6][2] = {{"ROOM", "NO ROOM"}, {"CRSH", "CRASHED"}, {"TIME", "TOO SLOW"},
							      {"MISS", "MISSING"}, {"BAD", "BAD"}, {"SAFE", "SAFE MODE"}};
			if (*t == '!') t++;
			const char *sp = strchr (t, ' '), *word = kWhy[st], *name = t;
			if (sp)
				for (int k = 0; k < 6; k++)
					if ((int) strlen (kTag[k][0]) == sp - t && !strncmp (t, kTag[k][0], (size_t) (sp - t))) word = kTag[k][1], name = sp + 1;
			lv_snprintf (b, sizeof b, "%s %s", word, name);
			if (th_text_width (lv_obj_get_style_text_font (s_USlot[i], 0), b) > (int) lv_obj_get_style_width (s_USlot[i], 0))
				lv_snprintf (b, sizeof b, "%s", word);		// (label_w: a plain width)
		}
		else
		{
			lv_snprintf (b, sizeof b, "%s", t);
			int n = (int) strlen (b);
			if (st == 6 && n >= 3 && !strcmp (b + n - 3, "...")) b[n - 3] = 0;	// (the dots move: uslot_paint)
		}
		if (strcmp (b, s_UText[i]) || st != s_UState[i])
		{
			if (st >= 2 && st <= 5 && !(s_UState[i] >= 2 && s_UState[i] <= 5)) s_UErrT0[i] = lv_tick_get ();
			lv_snprintf (s_UText[i], sizeof s_UText[i], "%s", b);
			s_UState[i] = st;
		}
	}
	uslot_paint ();

	if (s->touch_cal != s_nCalCal && s_CalState)
	{
		s_nCalCal = s->touch_cal;
		set_text (s_CalState, s->touch_cal ? (oled () ? "CALIBRATED" : "calibrated") : (oled () ? "DEFAULT" : "default (the device's range)"));
	}
	if (s->touch_hidden != s_nTouchSeen)				// a change of the stored setting, not our own tap
	{
		s_nTouchSeen = s->touch_hidden;
		touch_view (!s->touch_hidden);
	}
	lv_snprintf (b, sizeof b, "%s", s->ip[0] ? s->ip : "not up");
	set_text (s_Info[0], b);
	if (oled ()) lv_snprintf (b, sizeof b, "%s", s->host[0] ? s->host : "sidecar.local");
	else lv_snprintf (b, sizeof b, "http://%s/", s->host[0] ? s->host : "sidecar.local");
	set_text (s_Info[1], b);
	lv_snprintf (b, sizeof b, "%s%s%s", s->link, s->gadget[0] ? (oled () ? ", USB-C " : ", USB-C ") : "", s->gadget);
	set_text (s_Info[2], b);
	fmt_rec (b, sizeof b, s, oled () ? "OFF" : "off");
	set_text (s_Info[3], b);
	if (oled ()) th_label_color (s_Info[3], s->rec == 2 ? TH->bad : TH->ink);

	// the settings pages: only what changed reaches LVGL (and a hidden page costs no drawing)
	s_St = *s;
	s_bHaveSt = 1;
	stale_view ();
	if (s->fifo)							// set every time: a rebuild starts from "96 frames"
	{
		s_nFifo = s->fifo;
		lv_snprintf (b, sizeof b, "%u frames", s_nFifo);
		set_text (s_FifoVal, b);
	}
	seg_set (SEG_NOTES, s->notes ? 1 : 0);
	seg_set (SEG_OUT, s->out_tracks ? 1 : 0);
	seg_set (SEG_MAC, s->mac < 0 ? -1 : s->mac ? 1 : 0);
	th_seg_enable (s_Seg[SEG_MAC], -1, s->mac >= 0);
	voice_view ();
	for (int k = 0; k < 2; k++)
	{
		const char *t = s->fx_type[k][0] ? s->fx_type[k] : "-";
		const char *pp = s->fx_type[k][0] && strcmp (s->fx_type[k], "OFF") ? (s->fx_post[k] ? "  POST" : "  PRE") : "";
		lv_snprintf (b, sizeof b, "FX %d  %s%s", k + 1, t, pp);
		set_text (s_FxName[k], b);
		for (int t2 = 0; t2 < SIDECAR_UI_SENDS; t2++) th_meter_set (s_Bar[k][t2], s->send[k][t2]);
	}
	lv_snprintf (b, sizeof b, "http://%s/", s->host[0] ? s->host : "sidecar.local");
	set_text (s_About[0], b);
	set_text (s_About[1], s->host[0] ? s->host : "(mDNS not up yet)");
	set_text (s_About[2], s->ip);
	set_text (s_About[3], s->web ? (oled () ? "on (cmdline web=0: off)" : "on (cmdline web=0 turns it off; not switchable here)")
				    : "off (cmdline web=1)");
	set_text (s_About[4], s->link);
	set_text (s_About[5], s->gadget);
	set_text (s_About[6], s->build);
	lv_snprintf (b, sizeof b, "%s, LVGL %d.%d.%d", s->circle, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
	set_text (s_About[7], b);
	if (s->screensaver >= 0)
	{
		int k = -1;
		for (int i = 0; i < 4; i++) if (kSaverMin[i] == s->screensaver) k = i;
		seg_set (SEG_SAVER, k);
	}
	if (s->cpu_mhz > 0)
	{
		int k = -1;
		for (int i = 0; i < 3; i++) if (kClockMHz[i] == s->cpu_mhz) k = i;
		seg_set (SEG_CLOCK, k);
	}
	if (s->brightness >= 5 && !th_slider_dragging (s_Bright))
	{
		th_slider_set (s_Bright, s->brightness);
		lv_snprintf (b, sizeof b, "%d %%", s->brightness);
		set_text (s_BrightVal, b);
		sidecar_ui_dim (s->brightness);
	}
}

void sidecar_ui_set_command (sidecar_ui_command_t fn, void *param)
{
	s_Cmd = fn;
	s_CmdParam = param;
}

const char *sidecar_ui_banner (void)
{
	return s_Power && !lv_obj_has_flag (s_Power, LV_OBJ_FLAG_HIDDEN) ? s_Banner : 0;
}
int sidecar_ui_page (void) { return s_nPage; }
int sidecar_ui_nav_lit (void) { return s_nNavLit; }
const char *sidecar_ui_nav_page (int i) { return i >= 0 && i < NAV_N ? kNavPage[i] : 0; }
const char *sidecar_ui_nav_key (int i) { return i >= 0 && i < NAV_N ? nav_t (i) : 0; }
void sidecar_ui_combo_map (int on)
{
	on = !!on;
	if (on == s_nMap || !s_Map) { s_nMap = on; return; }
	s_nMap = on;
	if (on) lv_obj_remove_flag (s_Map, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Map, LV_OBJ_FLAG_HIDDEN);
}
int sidecar_ui_combo_map_shown (void) { return s_Map && !lv_obj_has_flag (s_Map, LV_OBJ_FLAG_HIDDEN); }
unsigned sidecar_ui_taps (void) { return s_nTaps; }
int sidecar_ui_touch_test_shown (void) { return s_nTouchShown; }
void sidecar_ui_touch_cal_start (void) { cal_cb (0, 0, 0); }
int sidecar_ui_touch_cal_step (void) { return s_nCalStep; }

void sidecar_ui_show (int page)
{
	show_page (page < 0 ? 0 : page >= PG_N ? PG_N - 1 : page);
}

void sidecar_ui_touch (int pressed, int raw_x, int raw_y, int x, int y)
{
	char b[48];
	lv_snprintf (b, sizeof b, "%d,%d > %d,%d%s", raw_x, raw_y, x, y, pressed ? "" : (oled () ? " UP" : " (up)"));
	set_text (s_Touch, b);
	set_text (s_Touch2, b);
	if (cal_touch (pressed, raw_x, raw_y, x, y)) return;
	// UX review T1: HOME's touch test goes once a tap was seen (the first one, at its release): stored as hidden; it
	// stays on Settings > Display
	if (pressed) s_bTouchDown = 1;
	else if (s_bTouchDown && s_nTouchShown)
	{
		s_bTouchDown = 0;
		touch_view (0);
		command ("touchtest=0");
	}
	if (pressed && s_nTouchShown)
	{
		int ux = x, uy = y, r = oled () ? 5 : 8;
		sidecar_ui_to_ui (&ux, &uy);
		lv_obj_set_pos (s_Dot, ux - r, uy - r);
		lv_obj_remove_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
	}
	else lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
}
