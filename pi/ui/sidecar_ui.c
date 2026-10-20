// pi/ui/sidecar_ui.c - see sidecar_ui.h. An 800x480 layout: the title bar, then either the home page (the 8 tracks, the
// status: Rytm, Overbridge, core load, and a touch test: a button, a slider, the touch coordinates and a dot where the
// finger is) or the settings pages (Sound: the web page's controls; Send FX: read only + kill tails; About). Every
// control sends a console command through the caller (sidecar_ui_set_command) and shows the state the caller reads back.
#include "sidecar_ui.h"
#include "lvgl.h"			// the LVGL tree's root (pi/lvgl-test/Makefile, host/Makefile)
#include "sidecar_wordmark.h"		// "Sidecar" in the docs site's wordmark face (wordmark.py)
#include <string.h>
#include <stdlib.h>

// the docs site's dark theme (Sidecar | Expander): neutral greys, one green accent. The greys are the docs' moved to
// the nearest that stay grey in RGB565 (G = 2 R; #0e0e0e, #1c1c1c, #ececec, #8f8f8f turn green there)
#define C_BG		0x101010
#define C_PANEL		0x181818
#define C_EDGE		0x383838
#define C_TEXT		0xe8e8e8
#define C_DIM		0x888888
#define C_ACCENT	0x2dbd5c
#define C_ACC_LO	0x0d3b1c	// pressed
#define C_ACC_HI	0x8be3a6
#define C_OK		C_ACCENT
#define C_BAD		0xff6b6b
#define C_FX2		C_TEXT		// Send FX: FX 1 in the accent, FX 2 in white

enum { SEG_TAB, SEG_NOTES, SEG_OUT, SEG_MAC, SEG_VOICE, SEG_MACH, SEG_DRONE, SEG_SAVER, SEG_TTEST, SEG_N };

static const char *const kTabMap[] = {"Sound", "Send FX", "About", ""};
static const char *const kOnOff[] = {"Off", "On", ""};
static const char *const kOutMap[] = {"Main", "Tracks", ""};
static const char *const kVoiceMap[] = {"1", "2", "3", "4", "5", "6", "7", "8", ""};
#define N_MACH 7
static const char *const kMachMap[] = {"PLAITS", "JUNO", "FM", "DAISY", "DSPS", "GRAIN", "DD", ""};
static const char *const kMachCmd[N_MACH] = {"plaits", "juno", "fm", "daisy", "dsps", "grain", "dd"};	// notemach=V:NAME
static const char *const kDroneMap[] = {"Off", "This voice", "All", ""};
static const char *const kSaverMap[] = {"Off", "15 min", "1 h", "4 h", ""};
static const int kSaverMin[4] = {0, 15, 60, 240};		// screensaver=MIN
static const char *const kSendName[SIDECAR_UI_SENDS] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};

static lv_obj_t *s_Rytm, *s_Blocks, *s_Underruns, *s_Note, *s_Rec;
static lv_obj_t *s_Project, *s_USlot[5];	// the user machines: the project, U1..U5
static lv_obj_t *s_Track[SIDECAR_UI_TRACKS];
static lv_obj_t *s_LoadBar[SIDECAR_UI_CORES], *s_LoadPct[SIDECAR_UI_CORES];
static lv_obj_t *s_Touch, *s_Taps, *s_SliderVal, *s_Dot;
static lv_obj_t *s_TouchPanel, *s_InfoPanel, *s_Info[4];	// the touch test, and what takes its place once hidden
static int s_nTouchShown = 1, s_nTouchSeen = -1;		// shown now; the caller's last value (-1: none yet)
static unsigned s_nTaps;
static int s_nRytm = -1;

static lv_obj_t *s_Home, *s_Pages, *s_Pg[3], *s_SetBtn, *s_SetBtnLabel;
static int s_nPage, s_nLast = 1;				// 0 home, 1..3 the settings pages
static sidecar_ui_command_t s_Cmd;
static void *s_CmdParam;
static sidecar_ui_status_t s_St;		// the last state given: the voice selector re-reads it
static int s_nVoice;				// the voice the machine and drone controls act on
static unsigned s_nFifo = 96;
static lv_obj_t *s_Seg[SEG_N], *s_FifoVal, *s_DroneInfo, *s_FxName[2], *s_Bar[2][SIDECAR_UI_SENDS], *s_Kill;
static lv_obj_t *s_About[8];
static lv_obj_t *s_Bright, *s_BrightVal, *s_Dim;
static int s_nDim = 100;
static int s_Sel[SEG_N];			// the checked button of each segmented control, -2: not set yet

static void set_text (lv_obj_t *l, const char *t)		// no redraw when nothing changed
{
	if (strcmp (lv_label_get_text (l), t) != 0) lv_label_set_text (l, t);
}

static lv_obj_t *panel (lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
	lv_obj_t *p = lv_obj_create (parent);
	lv_obj_remove_style_all (p);
	lv_obj_set_pos (p, x, y);
	lv_obj_set_size (p, w, h);
	lv_obj_set_style_bg_color (p, lv_color_hex (C_PANEL), 0);
	lv_obj_set_style_bg_opa (p, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color (p, lv_color_hex (C_EDGE), 0);
	lv_obj_set_style_border_width (p, 1, 0);
	lv_obj_set_style_radius (p, 4, 0);
	lv_obj_remove_flag (p, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *t = lv_label_create (p);
	lv_label_set_text (t, title);
	lv_obj_set_style_text_color (t, lv_color_hex (C_DIM), 0);
	lv_obj_set_style_text_font (t, &lv_font_montserrat_12, 0);
	lv_obj_set_style_text_letter_space (t, 2, 0);
	lv_obj_set_pos (t, 12, 8);
	return p;
}

static lv_obj_t *label (lv_obj_t *parent, int x, int y, const lv_font_t *f, unsigned color, const char *text)
{
	lv_obj_t *l = lv_label_create (parent);
	lv_label_set_text (l, text);
	lv_obj_set_style_text_font (l, f, 0);
	lv_obj_set_style_text_color (l, lv_color_hex (color), 0);
	lv_obj_set_pos (l, x, y);
	return l;
}

static void tap_cb (lv_event_t *e)
{
	(void) e;
	char b[24];
	lv_snprintf (b, sizeof b, "taps: %u", ++s_nTaps);
	set_text (s_Taps, b);
}

static void slider_cb (lv_event_t *e)
{
	char b[8];
	lv_snprintf (b, sizeof b, "%d", (int) lv_slider_get_value (lv_event_get_target_obj (e)));
	set_text (s_SliderVal, b);
}


static void command (const char *c)
{
	if (s_Cmd) s_Cmd (c, s_CmdParam);
}

static void seg_set (int id, int idx);

// The touch test panel or the info panel in its place; the dot goes with the panel
static void touch_view (int shown)
{
	s_nTouchShown = shown;
	if (shown) lv_obj_remove_flag (s_TouchPanel, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_TouchPanel, LV_OBJ_FLAG_HIDDEN);
	if (shown) lv_obj_add_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN);
	if (!shown) lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
	seg_set (SEG_TTEST, shown ? 1 : 0);
}

static void hide_cb (lv_event_t *e)
{
	(void) e;
	touch_view (0);
	command ("touchtest=0");
}

static lv_obj_t *transparent (lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create (parent);
	lv_obj_remove_style_all (o);
	lv_obj_set_pos (o, x, y);
	lv_obj_set_size (o, w, h);
	lv_obj_remove_flag (o, LV_OBJ_FLAG_SCROLLABLE);
	return o;
}

// A button of the given colours with a centred label
static lv_obj_t *button (lv_obj_t *parent, int x, int y, int w, int h, const char *text, unsigned bg, unsigned fg,
			 lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = lv_button_create (parent);
	lv_obj_set_pos (b, x, y);
	lv_obj_set_size (b, w, h);
	lv_obj_set_style_bg_color (b, lv_color_hex (bg), 0);
	lv_obj_set_style_bg_color (b, lv_color_hex (C_ACC_LO), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width (b, 0, 0);
	lv_obj_set_style_radius (b, 4, 0);
	lv_obj_add_event_cb (b, cb, LV_EVENT_CLICKED, ud);
	lv_obj_t *l = lv_label_create (b);
	lv_label_set_text (l, text);
	lv_obj_set_style_text_font (l, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color (l, lv_color_hex (fg), 0);
	lv_obj_center (l);
	return b;
}

static void show_page (int page)
{
	s_nPage = page;
	if (page) lv_obj_add_flag (s_Home, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag (s_Home, LV_OBJ_FLAG_HIDDEN);
	if (page) lv_obj_remove_flag (s_Pages, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Pages, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < 3; i++)
	{
		if (i == page - 1) lv_obj_remove_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
		else lv_obj_add_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
	}
	set_text (s_SetBtnLabel, page ? "Home" : "Settings");
	if (page)
	{
		s_nLast = page;
		lv_obj_remove_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);
		seg_set (SEG_TAB, page - 1);
	}
	else lv_obj_add_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);
}

// A segmented control: one button of the map checked at a time (a touch target of h >= 48 px each)
static lv_obj_t *seg (lv_obj_t *parent, int id, int x, int y, int w, const char *const *map, lv_event_cb_t cb)
{
	lv_obj_t *m = lv_buttonmatrix_create (parent);
	lv_buttonmatrix_set_map (m, map);
	lv_buttonmatrix_set_one_checked (m, true);
	lv_buttonmatrix_set_button_ctrl_all (m, LV_BUTTONMATRIX_CTRL_CHECKABLE);
	lv_obj_set_pos (m, x, y);
	lv_obj_set_size (m, w, 48);
	lv_obj_set_style_bg_opa (m, LV_OPA_TRANSP, LV_PART_MAIN);
	lv_obj_set_style_border_width (m, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all (m, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_gap (m, 4, LV_PART_MAIN);
	lv_obj_set_style_bg_color (m, lv_color_hex (C_BG), LV_PART_ITEMS);
	lv_obj_set_style_bg_opa (m, LV_OPA_COVER, LV_PART_ITEMS);
	lv_obj_set_style_border_color (m, lv_color_hex (C_EDGE), LV_PART_ITEMS);
	lv_obj_set_style_border_width (m, 1, LV_PART_ITEMS);
	lv_obj_set_style_shadow_width (m, 0, LV_PART_ITEMS);
	lv_obj_set_style_radius (m, 4, LV_PART_ITEMS);
	lv_obj_set_style_text_color (m, lv_color_hex (C_TEXT), LV_PART_ITEMS);
	lv_obj_set_style_text_font (m, id == SEG_MACH ? &lv_font_montserrat_14 : &lv_font_montserrat_16, LV_PART_ITEMS);
	lv_obj_set_style_bg_color (m, lv_color_hex (C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_text_color (m, lv_color_hex (C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color (m, lv_color_hex (C_ACC_LO), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_text_color (m, lv_color_hex (C_ACC_HI), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_text_color (m, lv_color_hex (C_DIM), LV_PART_ITEMS | LV_STATE_DISABLED);
	lv_obj_add_event_cb (m, cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);	// on release: a slide off the button cancels
	s_Seg[id] = m;
	s_Sel[id] = -2;
	return m;
}

// The checked button, -1: none. Only a change reaches LVGL.
static void seg_set (int id, int idx)
{
	if (s_Sel[id] == idx) return;
	lv_obj_t *m = s_Seg[id];
	if (s_Sel[id] >= 0) lv_buttonmatrix_clear_button_ctrl (m, s_Sel[id], LV_BUTTONMATRIX_CTRL_CHECKED);
	if (idx >= 0) lv_buttonmatrix_set_button_ctrl (m, idx, LV_BUTTONMATRIX_CTRL_CHECKED);
	s_Sel[id] = idx;
}

static void seg_enable (int id, int on)
{
	if (on) lv_obj_remove_state (s_Seg[id], LV_STATE_DISABLED); else lv_obj_add_state (s_Seg[id], LV_STATE_DISABLED);
}

static int same_name (const char *a, const char *b)		// ASCII, case-insensitive
{
	for (; *a && *b; a++, b++)
		if ((*a | 0x20) != (*b | 0x20)) return 0;
	return *a == *b;
}

// The machine and drone controls for s_nVoice, from the last state given
static void voice_view (void)
{
	const char *n = s_St.track[s_nVoice];
	int m = -1;
	if (n)
	{
		for (int k = 0; k < N_MACH; k++)
			if (same_name (n, kMachMap[k])) m = k;
		if (m == 4 && s_Sel[SEG_MACH] == 5) m = 5;		// DSPS and GRAIN share a machine: keep the one picked
	}
	seg_set (SEG_MACH, m);
	seg_set (SEG_DRONE, s_St.drone == -1 ? 0 : s_St.drone == s_nVoice ? 1 : s_St.drone == 8 ? 2 : -1);
	char b[40];
	if (s_St.drone == -1) lv_snprintf (b, sizeof b, "Drone");
	else if (s_St.drone == 8) lv_snprintf (b, sizeof b, "Drone: all voices");
	else lv_snprintf (b, sizeof b, "Drone: voice %d", s_St.drone + 1);
	set_text (s_DroneInfo, b);
}

static void seg_cb (lv_event_t *e)
{
	lv_obj_t *m = lv_event_get_target_obj (e);
	int id = (int) (intptr_t) lv_event_get_user_data (e);
	int i = (int) lv_buttonmatrix_get_selected_button (m);
	if (i == LV_BUTTONMATRIX_BUTTON_NONE) return;
	s_Sel[id] = i;							// the matrix has checked it already
	char b[40];
	switch (id)
	{
	case SEG_TAB:	show_page (i + 1); s_Sel[SEG_TAB] = i; break;
	case SEG_NOTES:	command (i ? "notes=1" : "notes=0"); break;
	case SEG_OUT:	command (i ? "out=tracks" : "out=main"); break;
	case SEG_MAC:	command (i ? "mac=on" : "mac=off"); break;
	case SEG_VOICE:	s_nVoice = i; s_Sel[SEG_MACH] = -2; voice_view (); break;
	case SEG_MACH:
		lv_snprintf (b, sizeof b, "notemach=%d:%s", s_nVoice, kMachCmd[i]);
		command (b);
		break;
	case SEG_SAVER:
		lv_snprintf (b, sizeof b, "screensaver=%d", kSaverMin[i]);
		command (b);
		break;
	case SEG_TTEST:
		touch_view (i);
		command (i ? "touchtest=1" : "touchtest=0");
		break;
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

static void fifo_cb (lv_event_t *e)
{
	int d = (int) (intptr_t) lv_event_get_user_data (e);
	int n = (int) s_nFifo + d * 8;
	s_nFifo = (unsigned) (n < 24 ? 24 : n > 1024 ? 1024 : n);
	char b[24];
	lv_snprintf (b, sizeof b, "fifo=%u", s_nFifo);
	command (b);
	lv_snprintf (b, sizeof b, "%u frames", s_nFifo);
	set_text (s_FifoVal, b);
}

static void kill_cb (lv_event_t *e)
{
	(void) e;
	command ("killfx");
}

static void settings_cb (lv_event_t *e)
{
	(void) e;
	show_page (s_nPage ? 0 : s_nLast);
}

static lv_obj_t *small (lv_obj_t *parent, int x, int y, const char *text)
{
	return label (parent, x, y, &lv_font_montserrat_14, C_DIM, text);
}

static void build_sound (lv_obj_t *pg)
{
	lv_obj_t *l = panel (pg, 12, 0, 380, 358, "ENGINE");
	small (l, 12, 30, "MIDI notes");
	seg (l, SEG_NOTES, 12, 48, 354, kOnOff, seg_cb);
	small (l, 12, 100, "Output");
	seg (l, SEG_OUT, 12, 118, 354, kOutMap, seg_cb);
	small (l, 12, 170, "Computer audio (USB-C)");
	seg (l, SEG_MAC, 12, 188, 354, kOnOff, seg_cb);
	small (l, 12, 240, "FIFO target");
	button (l, 12, 258, 96, 48, "-", C_EDGE, C_TEXT, fifo_cb, (void *) (intptr_t) -1);
	s_FifoVal = label (l, 120, 268, &lv_font_montserrat_20, C_TEXT, "96 frames");
	lv_obj_set_width (s_FifoVal, 146);
	lv_obj_set_style_text_align (s_FifoVal, LV_TEXT_ALIGN_CENTER, 0);
	button (l, 270, 258, 96, 48, "+", C_EDGE, C_TEXT, fifo_cb, (void *) (intptr_t) 1);

	lv_obj_t *r = panel (pg, 400, 0, 388, 358, "VOICE");
	small (r, 12, 30, "Voice");
	seg (r, SEG_VOICE, 12, 48, 364, kVoiceMap, seg_cb);
	small (r, 12, 100, "Machine (plays the MIDI notes)");
	seg (r, SEG_MACH, 12, 118, 364, kMachMap, seg_cb);
	s_DroneInfo = small (r, 12, 170, "Drone");
	seg (r, SEG_DRONE, 12, 188, 364, kDroneMap, seg_cb);
	lv_obj_t *h = small (r, 12, 252, "A drone is a steady test tone. The web page (sidecar.local) shows and sets the same.");
	lv_obj_set_width (h, 364);
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
	lv_buttonmatrix_set_button_ctrl (s_Seg[SEG_VOICE], 0, LV_BUTTONMATRIX_CTRL_CHECKED);
	s_Sel[SEG_VOICE] = 0;
}

static void build_fx (lv_obj_t *pg)
{
	lv_obj_t *p = panel (pg, 12, 0, 776, 358, "SEND FX  -  the Rytm's kit sets these, read only");
	for (int k = 0; k < 2; k++)
		s_FxName[k] = label (p, 12 + k * 250, 34, &lv_font_montserrat_24, k ? C_FX2 : C_ACCENT, "-");
	s_Kill = button (p, 584, 26, 180, 52, "Kill tails", C_BAD, C_TEXT, kill_cb, 0);
	for (int t = 0; t < SIDECAR_UI_SENDS; t++)
	{
		int x = 16 + t * 62;
		for (int k = 0; k < 2; k++)
		{
			lv_obj_t *b = lv_bar_create (p);
			lv_obj_set_pos (b, x + k * 26, 96);
			lv_obj_set_size (b, 22, 170);
			lv_bar_set_range (b, 0, 127);
			lv_obj_set_style_bg_color (b, lv_color_hex (C_EDGE), LV_PART_MAIN);
			lv_obj_set_style_bg_opa (b, LV_OPA_COVER, LV_PART_MAIN);
			lv_obj_set_style_bg_color (b, lv_color_hex (k ? C_FX2 : C_ACCENT), LV_PART_INDICATOR);
			lv_obj_set_style_radius (b, 2, LV_PART_MAIN);
			lv_obj_set_style_radius (b, 2, LV_PART_INDICATOR);
			s_Bar[k][t] = b;
		}
		label (p, x + 6, 272, &lv_font_montserrat_16, C_TEXT, kSendName[t]);
	}
	lv_obj_t *h = small (p, 12, 304, "Green: FX 1, white: FX 2, per track. A console override (sendlvl=, sendfx=) lasts only until "
			     "the kit's next message. Kill tails clears both FX at once (the Rytm's double STOP does the same).");
	lv_obj_set_width (h, 750);
	lv_label_set_long_mode (h, LV_LABEL_LONG_MODE_WRAP);
}

static const char *const kOnOffMap[] = {"Hide", "Show", ""};
static const char *const kAboutName[8] = {"Web page", "Address", "IP", "Web server", "Link", "USB-C", "Version", "Libraries"};

static void bright_cb (lv_event_t *e)
{
	int v = (int) lv_slider_get_value (lv_event_get_target_obj (e));
	char b[24];
	sidecar_ui_dim (v);						// live while dragging
	lv_snprintf (b, sizeof b, "%d %%", v);
	set_text (s_BrightVal, b);
	if (lv_event_get_code (e) == LV_EVENT_RELEASED)		// stored once, on release
	{
		lv_snprintf (b, sizeof b, "brightness=%d", v);
		command (b);
	}
}

static void build_about (lv_obj_t *pg)
{
	lv_obj_t *p = panel (pg, 12, 0, 776, 366, "ABOUT");
	for (int i = 0; i < 8; i++)
	{
		small (p, 16, 32 + i * 24, kAboutName[i]);
		s_About[i] = label (p, 190, 30 + i * 24, &lv_font_montserrat_16, C_TEXT, "-");
		lv_obj_set_width (s_About[i], 570);
		lv_label_set_long_mode (s_About[i], LV_LABEL_LONG_MODE_DOTS);
	}
	small (p, 16, 240, "Screensaver");
	seg (p, SEG_SAVER, 190, 226, 570, kSaverMap, seg_cb);
	small (p, 16, 294, "Touch test (home)");
	seg (p, SEG_TTEST, 190, 280, 200, kOnOffMap, seg_cb);
	small (p, 16, 342, "Brightness (dims the picture)");
	s_Bright = lv_slider_create (p);
	lv_obj_set_pos (s_Bright, 250, 342);
	lv_obj_set_size (s_Bright, 400, 18);
	lv_slider_set_range (s_Bright, 5, 100);
	lv_slider_set_value (s_Bright, 100, LV_ANIM_OFF);
	lv_obj_set_ext_click_area (s_Bright, 16);
	lv_obj_set_style_bg_color (s_Bright, lv_color_hex (C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color (s_Bright, lv_color_hex (C_TEXT), LV_PART_KNOB);
	lv_obj_add_event_cb (s_Bright, bright_cb, LV_EVENT_VALUE_CHANGED, 0);
	lv_obj_add_event_cb (s_Bright, bright_cb, LV_EVENT_RELEASED, 0);
	s_BrightVal = label (p, 672, 338, &lv_font_montserrat_16, C_TEXT, "100 %");
}

void sidecar_ui_dim (int percent)
{
	if (percent < 5) percent = 5;
	if (percent > 100) percent = 100;
	if (percent == s_nDim) return;
	s_nDim = percent;
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
	lv_obj_set_style_bg_opa (s_Dim, (lv_opa_t) ((100 - percent) * 255 * 70 / (95 * 100)), 0);
	if (percent >= 100) lv_obj_add_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
	else lv_obj_remove_flag (s_Dim, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_ui_create (void)
{
	lv_obj_t *scr = lv_screen_active ();
	lv_obj_set_style_bg_color (scr, lv_color_hex (C_BG), 0);
	lv_obj_set_style_bg_opa (scr, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color (scr, lv_color_hex (C_TEXT), 0);
	lv_obj_remove_flag (scr, LV_OBJ_FLAG_SCROLLABLE);

	s_Home = transparent (scr, 0, 0, 800, 480);		// below the title bar
	s_Pages = transparent (scr, 0, 114, 800, 366);	// the settings pages below their tab strip
	lv_obj_add_flag (s_Pages, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < 3; i++)
	{
		s_Pg[i] = transparent (s_Pages, 0, 0, 800, 366);
		lv_obj_add_flag (s_Pg[i], LV_OBJ_FLAG_HIDDEN);
	}

	// title bar
	lv_obj_t *bar = lv_obj_create (scr);
	lv_obj_remove_style_all (bar);
	lv_obj_set_pos (bar, 0, 0);
	lv_obj_set_size (bar, 800, 52);
	lv_obj_set_style_bg_color (bar, lv_color_hex (C_BG), 0);
	lv_obj_set_style_bg_opa (bar, LV_OPA_COVER, 0);
	lv_obj_set_style_border_side (bar, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color (bar, lv_color_hex (C_EDGE), 0);
	lv_obj_set_style_border_width (bar, 1, 0);
	lv_obj_remove_flag (bar, LV_OBJ_FLAG_SCROLLABLE);
	static const lv_image_dsc_t kWord = {
		.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8, .w = WORDMARK_A8_W, .h = WORDMARK_A8_H,
			   .stride = WORDMARK_A8_W},
		.data_size = sizeof WordmarkA8, .data = WordmarkA8};
	lv_obj_t *word = lv_image_create (bar);
	lv_image_set_src (word, &kWord);
	lv_obj_set_pos (word, 16, 11);
	lv_obj_set_style_image_recolor (word, lv_color_hex (C_TEXT), 0);
	lv_obj_set_style_image_recolor_opa (word, LV_OPA_COVER, 0);
	lv_obj_t *acc = lv_obj_create (bar);		// the favicon's green bar, under the "S"
	lv_obj_remove_style_all (acc);
	lv_obj_set_size (acc, 22, 3);
	lv_obj_set_pos (acc, 17, 11 + WORDMARK_A8_H + 3);
	lv_obj_set_style_bg_color (acc, lv_color_hex (C_ACCENT), 0);
	lv_obj_set_style_bg_opa (acc, LV_OPA_COVER, 0);
	lv_obj_t *rule = lv_obj_create (bar);		// the hairline between the two words
	lv_obj_remove_style_all (rule);
	lv_obj_set_size (rule, 1, 20);
	lv_obj_set_pos (rule, 16 + WORDMARK_A8_W + 10, 15);
	lv_obj_set_style_bg_color (rule, lv_color_hex (C_EDGE), 0);
	lv_obj_set_style_bg_opa (rule, LV_OPA_COVER, 0);
	lv_obj_t *sub = label (bar, 16 + WORDMARK_A8_W + 21, 18, &lv_font_montserrat_12, C_DIM, "EXPANDER");
	lv_obj_set_style_text_letter_space (sub, 4, 0);
	s_Rytm = label (bar, 330, 17, &lv_font_montserrat_16, C_DIM, "");
	lv_obj_set_width (s_Rytm, 296);
	lv_obj_set_style_text_align (s_Rytm, LV_TEXT_ALIGN_RIGHT, 0);

	s_SetBtn = button (bar, 640, 1, 148, 48, "Settings", C_EDGE, C_TEXT, settings_cb, 0);
	s_SetBtnLabel = lv_obj_get_child (s_SetBtn, 0);

	// the settings pages, each under the tab strip
	seg (scr, SEG_TAB, 12, 60, 776, kTabMap, seg_cb);
	lv_obj_add_flag (s_Seg[SEG_TAB], LV_OBJ_FLAG_HIDDEN);
	build_sound (s_Pg[0]);
	build_fx (s_Pg[1]);
	build_about (s_Pg[2]);

	// tracks: 2 columns of 4
	lv_obj_t *tp = panel (s_Home, 12, 64, 476, 232, "TRACKS");
	for (int i = 0; i < SIDECAR_UI_TRACKS; i++)
	{
		int x = 12 + (i / 4) * 230, y = 32 + (i % 4) * 48;
		lv_obj_t *t = lv_obj_create (tp);
		lv_obj_remove_style_all (t);
		lv_obj_set_pos (t, x, y);
		lv_obj_set_size (t, 220, 42);
		lv_obj_set_style_bg_color (t, lv_color_hex (C_BG), 0);
		lv_obj_set_style_bg_opa (t, LV_OPA_COVER, 0);
		lv_obj_set_style_radius (t, 4, 0);
		lv_obj_remove_flag (t, LV_OBJ_FLAG_SCROLLABLE);
		char n[4];
		lv_snprintf (n, sizeof n, "%d", i + 1);
		label (t, 10, 11, &lv_font_montserrat_16, C_ACCENT, n);
		s_Track[i] = label (t, 36, 12, &lv_font_montserrat_16, C_TEXT, "-");
		lv_obj_set_width (s_Track[i], 176);
		lv_label_set_long_mode (s_Track[i], LV_LABEL_LONG_MODE_DOTS);
	}

	// status
	lv_obj_t *sp = panel (s_Home, 500, 64, 288, 232, "STATUS");
	label (sp, 12, 32, &lv_font_montserrat_14, C_DIM, "Overbridge blocks");
	s_Blocks = label (sp, 160, 32, &lv_font_montserrat_14, C_TEXT, "-");
	label (sp, 12, 56, &lv_font_montserrat_14, C_DIM, "Underruns");
	s_Underruns = label (sp, 160, 56, &lv_font_montserrat_14, C_TEXT, "-");
	s_Rec = label (sp, 150, 4, &lv_font_montserrat_20, C_BAD, "");	// the recorder: a red REC and the take's time
	for (int c = 0; c < SIDECAR_UI_CORES; c++)
	{
		int y = 92 + c * 30;
		char n[12];
		lv_snprintf (n, sizeof n, "core %d", c);
		label (sp, 12, y, &lv_font_montserrat_14, C_DIM, n);
		lv_obj_t *b = lv_bar_create (sp);
		lv_obj_set_pos (b, 76, y + 3);
		lv_obj_set_size (b, 140, 12);
		lv_bar_set_range (b, 0, 100);
		lv_obj_set_style_bg_color (b, lv_color_hex (C_EDGE), LV_PART_MAIN);
		lv_obj_set_style_bg_opa (b, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_bg_color (b, lv_color_hex (C_ACCENT), LV_PART_INDICATOR);
		s_LoadBar[c] = b;
		s_LoadPct[c] = label (sp, 226, y, &lv_font_montserrat_14, C_TEXT, "-");
	}

	// the user machines: the Rytm's project and what U1..U5 play (core/user/um.h UiView)
	lv_obj_t *up = panel (s_Home, 404, 308, 384, 160, "PROJECT");
	s_Project = label (up, 12, 30, &lv_font_montserrat_16, C_TEXT, "-");
	lv_obj_set_width (s_Project, 360);
	lv_label_set_long_mode (s_Project, LV_LABEL_LONG_MODE_DOTS);
	for (int i = 0; i < 5; i++)
	{
		int x = 12 + (i % 3) * 122, y = 62 + (i / 3) * 44;
		char n[4];
		lv_snprintf (n, sizeof n, "U%d", i + 1);
		label (up, x, y, &lv_font_montserrat_12, C_ACCENT, n);
		s_USlot[i] = label (up, x, y + 16, &lv_font_montserrat_14, C_DIM, "-");
		lv_obj_set_width (s_USlot[i], 114);
		lv_label_set_long_mode (s_USlot[i], LV_LABEL_LONG_MODE_DOTS);
	}

	// touch test (until the user hides it: touchtest=0), and in its place what the display already knows
	lv_obj_t *xp = panel (s_Home, 12, 308, 380, 160, "TOUCH TEST");
	s_TouchPanel = xp;
	lv_obj_t *btn = lv_button_create (xp);
	lv_obj_set_pos (btn, 12, 32);
	lv_obj_set_size (btn, 110, 48);
	lv_obj_set_style_bg_color (btn, lv_color_hex (C_ACCENT), 0);
	lv_obj_set_style_bg_color (btn, lv_color_hex (C_ACC_HI), LV_STATE_PRESSED);
	lv_obj_set_style_radius (btn, 4, 0);
	lv_obj_set_style_shadow_width (btn, 0, 0);
	lv_obj_add_event_cb (btn, tap_cb, LV_EVENT_CLICKED, 0);
	lv_obj_t *bl = lv_label_create (btn);
	lv_label_set_text (bl, "TAP");
	lv_obj_set_style_text_font (bl, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color (bl, lv_color_hex (C_BG), 0);
	lv_obj_center (bl);
	s_Taps = label (xp, 12, 84, &lv_font_montserrat_14, C_TEXT, "taps: 0");

	lv_obj_t *sl = lv_slider_create (xp);
	lv_obj_set_pos (sl, 140, 40);
	lv_obj_set_size (sl, 170, 20);
	lv_slider_set_range (sl, 0, 127);
	lv_obj_set_style_bg_color (sl, lv_color_hex (C_EDGE), LV_PART_MAIN);
	lv_obj_set_style_bg_opa (sl, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_bg_color (sl, lv_color_hex (C_ACCENT), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color (sl, lv_color_hex (C_TEXT), LV_PART_KNOB);
	lv_obj_add_event_cb (sl, slider_cb, LV_EVENT_VALUE_CHANGED, 0);
	s_SliderVal = label (xp, 322, 38, &lv_font_montserrat_20, C_TEXT, "0");

	label (xp, 140, 62, &lv_font_montserrat_12, C_DIM, "raw -> screen");
	s_Touch = label (xp, 140, 78, &lv_font_montserrat_14, C_TEXT, "no touch yet");
	s_Note = label (xp, 150, 8, &lv_font_montserrat_12, C_DIM, "");
	lv_obj_set_width (s_Note, 218);
	lv_obj_set_style_text_align (s_Note, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode (s_Note, LV_LABEL_LONG_MODE_DOTS);
	button (xp, 12, 104, 354, 48, "Touch works - hide", C_EDGE, C_TEXT, hide_cb, 0);

	s_InfoPanel = panel (s_Home, 12, 308, 380, 160, "SIDECAR");
	static const char *const kInfoName[4] = {"Network", "Web page", "Link", "Recorder"};
	for (int i = 0; i < 4; i++)
	{
		label (s_InfoPanel, 12, 36 + i * 28, &lv_font_montserrat_14, C_DIM, kInfoName[i]);
		s_Info[i] = label (s_InfoPanel, 110, 34 + i * 28, &lv_font_montserrat_16, C_TEXT, "-");
		lv_obj_set_width (s_Info[i], 258);
		lv_label_set_long_mode (s_Info[i], LV_LABEL_LONG_MODE_DOTS);
	}
	lv_obj_add_flag (s_InfoPanel, LV_OBJ_FLAG_HIDDEN);

	// the dot under the finger: on the top layer, never clickable
	s_Dot = lv_obj_create (lv_layer_top ());
	lv_obj_remove_style_all (s_Dot);
	lv_obj_set_size (s_Dot, 16, 16);
	lv_obj_set_style_radius (s_Dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_color (s_Dot, lv_color_hex (C_OK), 0);
	lv_obj_set_style_border_width (s_Dot, 2, 0);
	lv_obj_remove_flag (s_Dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_ui_status (const sidecar_ui_status_t *s)
{
	char b[64];
	if (s->rytm != s_nRytm)
	{
		s_nRytm = s->rytm;
		set_text (s_Rytm, s->rytm ? "Rytm connected" : "Rytm not connected");
		lv_obj_set_style_text_color (s_Rytm, lv_color_hex (s->rytm ? C_OK : C_BAD), 0);
	}
	lv_snprintf (b, sizeof b, "%u", s->blocks);
	set_text (s_Blocks, b);
	lv_snprintf (b, sizeof b, "%u", s->underruns);
	set_text (s_Underruns, b);
	lv_obj_set_style_text_color (s_Underruns, lv_color_hex (s->underruns ? C_BAD : C_TEXT), 0);
	if (s->rec == 2) lv_snprintf (b, sizeof b, "REC %u:%02u:%02u", s->rec_secs / 3600, s->rec_secs / 60 % 60, s->rec_secs % 60);
	else if (s->rec == 1) lv_snprintf (b, sizeof b, "REC armed");
	else b[0] = 0;
	set_text (s_Rec, b);
	lv_obj_set_style_text_color (s_Rec, lv_color_hex (s->rec == 2 ? C_BAD : C_DIM), 0);
	for (int c = 0; c < SIDECAR_UI_CORES; c++)
	{
		int l = s->load[c];
		if (l < 0) set_text (s_LoadPct[c], "-");
		else
		{
			lv_snprintf (b, sizeof b, "%d%%", l);
			set_text (s_LoadPct[c], b);
		}
		lv_bar_set_value (s_LoadBar[c], l < 0 ? 0 : l > 100 ? 100 : l, LV_ANIM_OFF);
	}
	for (int i = 0; i < SIDECAR_UI_TRACKS; i++)
		set_text (s_Track[i], s->track[i] && s->track[i][0] ? s->track[i] : "-");
	set_text (s_Note, s->note ? s->note : "");
	static int s_nNoteBad = -1;		// the supply warning (power_state.h) in red; restyled only on a change
	int nNoteBad = s->note && strncmp (s->note, "POWER", 5) == 0;
	if (nNoteBad != s_nNoteBad) lv_obj_set_style_text_color (s_Note, lv_color_hex (nNoteBad ? C_BAD : C_DIM), 0);
	s_nNoteBad = nNoteBad;
	set_text (s_Project, s->project[0] ? s->project : "-");
	for (int i = 0; i < 5; i++)
	{
		int st = s->uslot_state[i];
		set_text (s_USlot[i], s->uslot[i][0] ? s->uslot[i] : "-");
		lv_obj_set_style_text_color (s_USlot[i], lv_color_hex (st == 1 ? C_TEXT : st == 0 ? C_DIM : st == 6 ? C_ACCENT : C_BAD), 0);
	}

	if (s->touch_hidden != s_nTouchSeen)				// a change of the stored setting, not our own tap
	{
		s_nTouchSeen = s->touch_hidden;
		touch_view (!s->touch_hidden);
	}
	lv_snprintf (b, sizeof b, "%s", s->ip[0] ? s->ip : "not up");
	set_text (s_Info[0], b);
	lv_snprintf (b, sizeof b, "http://%s/", s->host[0] ? s->host : "sidecar.local");
	set_text (s_Info[1], b);
	lv_snprintf (b, sizeof b, "%s%s%s", s->link, s->gadget[0] ? ", USB-C " : "", s->gadget);
	set_text (s_Info[2], b);
	if (s->rec == 2) lv_snprintf (b, sizeof b, "REC %u:%02u:%02u", s->rec_secs / 3600, s->rec_secs / 60 % 60, s->rec_secs % 60);
	else lv_snprintf (b, sizeof b, "%s", s->rec == 1 ? "armed" : "off");
	set_text (s_Info[3], b);

	// the settings pages: only what changed reaches LVGL (and a hidden page costs no drawing)
	s_St = *s;
	if (s_nFifo != s->fifo && s->fifo)
	{
		s_nFifo = s->fifo;
		lv_snprintf (b, sizeof b, "%u frames", s_nFifo);
		set_text (s_FifoVal, b);
	}
	seg_set (SEG_NOTES, s->notes ? 1 : 0);
	seg_set (SEG_OUT, s->out_tracks ? 1 : 0);
	if (s->mac < 0) seg_set (SEG_MAC, -1);
	else seg_set (SEG_MAC, s->mac ? 1 : 0);
	seg_enable (SEG_MAC, s->mac >= 0);
	voice_view ();
	for (int k = 0; k < 2; k++)
	{
		lv_snprintf (b, sizeof b, "FX %d  %s%s", k + 1, s->fx_type[k][0] ? s->fx_type[k] : "-",
			    s->fx_type[k][0] && strcmp (s->fx_type[k], "OFF") ? (s->fx_post[k] ? "  POST" : "  PRE") : "");
		set_text (s_FxName[k], b);
		for (int t = 0; t < SIDECAR_UI_SENDS; t++)
			if (lv_bar_get_value (s_Bar[k][t]) != s->send[k][t]) lv_bar_set_value (s_Bar[k][t], s->send[k][t], LV_ANIM_OFF);
	}
	lv_snprintf (b, sizeof b, "http://%s/", s->host[0] ? s->host : "sidecar.local");
	set_text (s_About[0], b);
	set_text (s_About[1], s->host[0] ? s->host : "(mDNS not up yet)");
	set_text (s_About[2], s->ip);
	set_text (s_About[3], s->web ? "on (cmdline web=0 turns it off; not switchable here)" : "off (cmdline web=1)");
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
	if (s->brightness >= 5 && !lv_obj_has_state (s_Bright, LV_STATE_PRESSED))
	{
		if ((int) lv_slider_get_value (s_Bright) != s->brightness)
		{
			lv_slider_set_value (s_Bright, s->brightness, LV_ANIM_OFF);
			lv_snprintf (b, sizeof b, "%d %%", s->brightness);
			set_text (s_BrightVal, b);
		}
		sidecar_ui_dim (s->brightness);
	}
}

void sidecar_ui_set_command (sidecar_ui_command_t fn, void *param)
{
	s_Cmd = fn;
	s_CmdParam = param;
}

void sidecar_ui_show (int page)
{
	show_page (page < 0 ? 0 : page > 3 ? 3 : page);
}

void sidecar_ui_touch (int pressed, int raw_x, int raw_y, int x, int y)
{
	char b[48];
	lv_snprintf (b, sizeof b, "%d,%d -> %d,%d%s", raw_x, raw_y, x, y, pressed ? "" : " (up)");
	set_text (s_Touch, b);
	if (pressed && s_nTouchShown)
	{
		lv_obj_set_pos (s_Dot, x - 8, y - 8);
		lv_obj_remove_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
	}
	else lv_obj_add_flag (s_Dot, LV_OBJ_FLAG_HIDDEN);
}
