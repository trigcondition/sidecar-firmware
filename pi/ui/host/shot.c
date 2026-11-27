// pi/ui/host/shot.c - the Pi screen on the Mac: LVGL renders sidecar_ui into an 800x480 RGB565 buffer (the OLED theme's
// dots expanded as the Pi's flush expands them: sidecar_dot_expand), with made-up status and touches, written as PNGs.
// No window, no SDL.
//   ./shot OUTDIR [WxH]      every page in both themes (WxH: the screen, 800x480, 1024x600 or 1280x720) (OUTDIR/oled-*.png, OUTDIR/sidecar-*.png), the controls' commands
//                            checked, the SEQ gestures checked against core/seq, the draw costs printed; fails on a miss
//   ./shot anim OUTDIR [N]   the SEQ piano roll playing in the OLED theme: N frames (default 300) at 20 fps, frame-NNNN.png
#include <zlib.h>
#include "lvgl.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/widgets/label/lv_label_private.h"	// dot_begin: a DOTS label cut (the text-fit check)
#include "../sidecar_ui.h"
#include "../sidecar_dot.h"
#include "../sidecar_theme.h"
#include "../sidecar_voice.h"
#include "../sidecar_insfx.h"
#include "../sidecar_mfx.h"
#include "../sidecar_morph.h"
#include "../sidecar_loop.h"
#include "../sidecar_xy.h"
#include "../sidecar_song.h"
#include "../sidecar_rec.h"
#include "../sidecar_seqed.h"
#include "../sidecar_rytmpat.h"
#include "../sidecar_wgview.h"
#include "../sidecar_preset.h"
#include "../sidecar_scope.h"
#include "../sidecar_chop.h"
#include "../sidecar_sedit.h"
#include "../sidecar_status.h"
#include "../sidecar_project.h"
#include "../../../core/sidecar_features.h"	// SIDECAR_MORPH: the FX row has no Morph pill without it
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WMAX 1280
#define HMAX 720
static int W = 800, H = 480;			// the screen (./shot OUTDIR 1024x600: the wide layouts)
static uint16_t s_Fb[WMAX * HMAX];
static uint16_t s_Buf[WMAX * HMAX / 10];
static uint16_t s_DotsStatic[WMAX * 16 * SIDECAR_DOT];	// a band of expanded dots
static uint16_t *s_Dots = s_DotsStatic;	// fuzz: a malloc of exactly the Pi's m_pDotBuf size (W * DotRows * SIDECAR_DOT), so ASan sees an overrun
int fz_blit (uint16_t *fb, unsigned fx, unsigned fy, unsigned w, unsigned h, const uint16_t *src, unsigned stride);
static uint16_t *s_Panel;		// fuzz: the Pi 5 panel frame buffer, 720x1280 exact (rotation 270), written by dsirot::blit as PutFrame does
static long s_nBlitMiss;
static void panel_put (unsigned fx, unsigned fy, unsigned w, unsigned h, const uint16_t *src, unsigned stride)
{
	if (!s_Panel) return;
	if (!fz_blit (s_Panel, fx, fy, w, h, src, stride)) s_nBlitMiss++;
}

// as CSidecarDisplay's flush: UI pixels straight through, or each dot expanded (sidecar_ui_dot)
static void flush (lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const uint16_t *p = (const uint16_t *) px;
	int dot = sidecar_ui_dot ();
	int aw = a->x2 - a->x1 + 1, ah = a->y2 - a->y1 + 1;
	if (dot == 1)
	{
		int ox, oy;
		sidecar_ui_origin (&ox, &oy);
		panel_put (ox + a->x1, oy + a->y1, aw, ah, p, aw);
		for (int y = a->y1; y <= a->y2; y++)
			for (int x = a->x1; x <= a->x2; x++) s_Fb[(oy + y) * W + ox + x] = *p++;
		lv_display_flush_ready (d);
		return;
	}
	int ox, oy;
	sidecar_ui_origin (&ox, &oy);
	for (int y0 = 0; y0 < ah; y0 += 16)
	{
		int n = ah - y0 < 16 ? ah - y0 : 16;
		sidecar_dot_expand (p + y0 * aw, a->x1, a->y1 + y0, aw, n, aw, s_Dots, aw * dot, 0, sidecar_ui_bright ());
		panel_put (ox + a->x1 * dot, oy + (a->y1 + y0) * dot, aw * dot, n * dot, s_Dots, aw * dot);
		for (int r = 0; r < n * dot; r++)
		{
			int fy = oy + (a->y1 + y0) * dot + r;
			memcpy (&s_Fb[fy * W + ox + a->x1 * dot], &s_Dots[r * aw * dot], (size_t) aw * dot * 2);
		}
	}
	lv_display_flush_ready (d);
}

// the SEQ page's model (seqdemo.cpp: core/seq's pattern through seq_touch) and the Rytm view's (rytm_fake.c)
void seqdemo_build (void);
int seqdemo_command (const char *line);
void seqdemo_view (sidecar_seq_view_t *v, int playing);
int seqdemo_note (int rytm, int step, int pitch, int *vel, int *micro);
int seqdemo_swing (int rytm);
int seqdemo_poly (int rytm);
int seqdemo_x (int rytm, int *pat);
int seqdemo_xstep (int rytm, int step, int *fx, int *lc, int *rnd);
int seqdemo_arpl (int rytm, int step, int f);
int seqdemo_seed (void);
int seqdemo_rytm_parse (const unsigned char *m, int n, unsigned char *rec, int *pattern);

// a stock pattern dump of rec (sim/protocol.py rytm_dump, core/tests/test_seq.cpp dump_of): what the Rytm answers
static int rytm_dump_of (const unsigned char *rec, int pattern, unsigned char *m, int cap)
{
	static const unsigned char kHead[9] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0x54, 0x01, 0x01};
	memcpy (m, kHead, 9);
	m[9] = (unsigned char) pattern;
	int n = 10 + rytm_sysex_pack (rec, RYTM_PAT_BYTES, m + 10, cap - 15);	// groups from byte 10, the header first (0.5.0 FILL)
	unsigned cs = 0;
	for (int i = 10; i < n; i++) cs += m[i];	// (the pattern number not in it: core/seq/rytm_dump, 641f794)
	m[n++] = (cs >> 7) & 0x7F, m[n++] = cs & 0x7F;
	int len = n + 3 - 10;
	m[n++] = (len >> 7) & 0x7F, m[n++] = len & 0x7F, m[n++] = 0xF7;
	return n;
}
void rytm_fake (rytm_pattern_t *p);
static sidecar_seq_view_t s_View;
static char s_Last[200];
static int s_nCmds;		// commands sent (several at once: SELECT UNUSED's UNLOAD)
static char s_Hist[256][64];	// the last 256 of them, each (the touch edits: every line of a drag), line i at i & 255
static int s_bQuiet;
static int s_nXyPos;		// 0.5.0: the XY pad's xy=pos lines (a drag's, throttled)
static int s_nXyUp;		// its xy=up lines
static char s_XyPos[40];	// the last xy=pos line

static int s_SeqPlay = 1;	// the made-up Rytm plays (the SEQ page's T5 VIEW)
int rmt_cmd_allowed (const char *line);	// the remote UI service's allowlist (allow_shim.cpp, pi/engine/remote_srv.cpp)
static int s_nFail;
static void on_command (const char *c, void *p)
{
	(void) p;
	if (!s_bQuiet) printf ("command: %s\n", c);
	snprintf (s_Last, sizeof s_Last, "%s", c);
	snprintf (s_Hist[s_nCmds & 255], sizeof s_Hist[0], "%s", c);
	s_nCmds++;
	if (strncmp (c, "xy=pos:", 7) == 0) s_nXyPos++, snprintf (s_XyPos, sizeof s_XyPos, "%s", c);
	if (strcmp (c, "xy=up") == 0) s_nXyUp++;
	if (strncmp (c, "seqed=", 6) == 0 || strncmp (c, "seq=", 4) == 0)	// as core 3 would: applied, the view published back
	{
		if (!rmt_cmd_allowed (c)) { printf ("FAIL shot.c: the remote UI's allowlist refuses a line the SEQ page sends: %s\n", c); s_nFail++; }
		seqdemo_command (c);
		seqdemo_view (&s_View, s_SeqPlay);
		sidecar_seqed_view (&s_View);
	}
}

// a finger at (x,y) in screen pixels: pressed for a few LVGL ticks, then lifted
static int s_Px, s_Py, s_Down;
static void pointer (lv_indev_t *i, lv_indev_data_t *d)
{
	(void) i;
	int x = s_Px, y = s_Py;
	sidecar_ui_to_ui (&x, &y);				// as the Pi's PointerRead
	d->state = s_Down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	d->point.x = x;
	d->point.y = y;
}
static int s_nTickMs = 40;	// the ms a run () step takes (the touch edits' throttle test: 10)
static void run (int n) { for (int i = 0; i < n; i++) { lv_tick_inc (s_nTickMs); lv_timer_handler (); } }
// UI coordinates -> the screen (the middle of the dot)
static int sx (int x) { int ox, oy; sidecar_ui_origin (&ox, &oy); return ox + x * sidecar_ui_dot () + sidecar_ui_dot () / 2; }
static int sy (int y) { int ox, oy; sidecar_ui_origin (&ox, &oy); return oy + y * sidecar_ui_dot () + sidecar_ui_dot () / 2; }
// 0.5.1: the pages below a section's sub-page row (everything but HOME, SEQ, PAT and the settings pages) sit TH->dy - TH->pdy lower
// than the 800x480 layouts the tests were written in (their LU (52) origin and DY): tap, hold and drag move the y of a tap
// on them by that, and DY reads TH->pdy there. tap_raw: the screen's own coordinates (the navigation, the sub-page row)
static int mod_page (void)
{
	switch (sidecar_ui_page ())
	{
	case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 16: case 23: case 24: return 0;
	}
	return 1;
}
static int s_bRaw;							// 1: the next drag's coordinates are the screen's (a page's own _hit)
#define MY()	(s_bRaw ? 0 : mod_page () ? (TH->id == THEME_OLED ? LU (25) : LU (58)) : 0)
#define DY	(mod_page () ? TH->pdy : TH->dy)
static void tap_raw (int x, int y)
{
	s_Px = sx (x); s_Py = sy (y); s_Down = 1; run (3);
	s_Down = 0; run (3);
}
static void tap (int x, int y) { tap_raw (x, y + MY ()); }	// UI coordinates
static void hold (int x, int y)				// a long press (LVGL's 400 ms)
{
	s_Px = sx (x); s_Py = sy (y + MY ()); s_Down = 1; run (15);
	s_Down = 0; run (3);
}

// a frame as an 8-bit RGB PNG (deflate from the system's libz, filter 0: these frames are flat colour, they pack small).
// SHOT_NOFRAMES=1 in the environment writes none (the bounds run only needs the .bounds files).
static void put32 (unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void png_chunk (FILE *f, const char *type, const unsigned char *d, uint32_t n)
{
	unsigned char h[8];
	put32 (h, n);
	memcpy (h + 4, type, 4);
	uLong crc = crc32 (0, h + 4, 4);
	if (n) crc = crc32 (crc, d, n);					// crc32 (c, NULL, 0) returns 0, not c: IEND would get a zero CRC
	if (fwrite (h, 1, 8, f) != 8 || (n && fwrite (d, 1, n, f) != n)) { perror ("png write"); exit (1); }
	put32 (h, (uint32_t) crc);
	if (fwrite (h, 1, 4, f) != 4) { perror ("png write"); exit (1); }
}
static void write_png (const char *name)
{
	if (getenv ("SHOT_NOFRAMES")) return;
	const size_t row = (size_t) W * 3 + 1, n = row * H;
	unsigned char *raw = malloc (n);
	uLongf zn = compressBound ((uLong) n);
	unsigned char *z = malloc (zn);
	if (!raw || !z) { perror ("png"); exit (1); }
	for (int y = 0; y < H; y++)
	{
		unsigned char *r = raw + row * y;
		*r++ = 0;
		for (int x = 0; x < W; x++)
		{
			uint16_t c = s_Fb[y * W + x];
			*r++ = (unsigned char) ((c >> 11) << 3);
			*r++ = (unsigned char) (((c >> 5) & 63) << 2);
			*r++ = (unsigned char) ((c & 31) << 3);
		}
	}
	if (compress2 (z, &zn, raw, (uLong) n, 1) != Z_OK) { fprintf (stderr, "%s: deflate failed\n", name); exit (1); }
	FILE *f = fopen (name, "wb");
	if (!f) { perror (name); exit (1); }
	unsigned char ihdr[13] = {0};
	put32 (ihdr, W);
	put32 (ihdr + 4, H);
	ihdr[8] = 8, ihdr[9] = 2;						// 8 bit, RGB
	if (fwrite ("\x89PNG\r\n\x1a\n", 1, 8, f) != 8) { perror (name); exit (1); }
	png_chunk (f, "IHDR", ihdr, 13);
	png_chunk (f, "IDAT", z, (uint32_t) zn);
	png_chunk (f, "IEND", NULL, 0);
	if (fclose (f) != 0) { perror (name); exit (1); }
	free (raw);
	free (z);
}

static char s_Dir[256], s_Pre[16];
// ---- the text-fit check (UX review R1): every string on the screen fits its box. Painters report through
// th_fit_report (sidecar_theme.c); labels are walked here at each shot: a CLIP label narrower than its text, a WRAP
// label taller than its parent leaves it room, a DOTS label cut to "..." (names are shortened in the middle before they
// are set: th_label_name), any label outside its parent. Each failure once, by text.
static int s_nFit;
static char s_FitSeen[400][48];
static int s_nFitSeen;
static char s_ShotName[48];
static void fit_fail (const char *t, int need, int have, const char *why)
{
	char k[48];
	snprintf (k, sizeof k, "%s", t);
	for (int i = 0; i < s_nFitSeen; i++) if (!strcmp (s_FitSeen[i], k)) return;
	if (s_nFitSeen < 400) snprintf (s_FitSeen[s_nFitSeen++], sizeof s_FitSeen[0], "%s", k);
	printf ("TEXT-FIT [%s-%s %dx%d] \"%s\" %s: needs %d px, has %d\n", s_Pre, s_ShotName, W, H, t, why, need, have);
	s_nFit++;
}
static void fit_painted (const char *t, int need, int have) { fit_fail (t, need, have, "painted"); }
static void fit_walk (lv_obj_t *o)
{
	if (lv_obj_has_flag (o, LV_OBJ_FLAG_HIDDEN)) return;
	if (lv_obj_check_type (o, &lv_label_class))
	{
		const char *t = lv_label_get_text (o);
		if (t && t[0])
		{
			const lv_font_t *f = lv_obj_get_style_text_font (o, 0);
			int ls = lv_obj_get_style_text_letter_space (o, 0), sp = lv_obj_get_style_text_line_space (o, 0);
			lv_area_t c, pc;
			lv_obj_get_coords (o, &c);
			lv_obj_get_coords (lv_obj_get_parent (o), &pc);
			int w = lv_area_get_width (&c), mode = lv_label_get_long_mode (o);
			lv_point_t sz;
			if (mode == LV_LABEL_LONG_MODE_DOTS && ((lv_label_t *) o)->dot_begin != 0xFFFFFFFFu)	// (lv_label.c LV_LABEL_DOT_BEGIN_INV)
				fit_fail (t, w + 1, w, "ellipsized");
			else if (mode == LV_LABEL_LONG_MODE_WRAP)
			{
				lv_text_get_size (&sz, t, f, ls, sp, w, LV_TEXT_FLAG_NONE);
				if (c.y1 + sz.y - 1 > pc.y2) fit_fail (t, sz.y, pc.y2 - c.y1 + 1, "wraps below its panel");
			}
			else
			{
				lv_text_get_size (&sz, t, f, ls, sp, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
				if (sz.x > w) fit_fail (t, sz.x, w, mode == LV_LABEL_LONG_MODE_DOTS ? "ellipsized" : "clipped");
				else if (c.x1 + sz.x - 1 > pc.x2) fit_fail (t, sz.x, pc.x2 - c.x1 + 1, "past its panel");
				else if (c.y2 > pc.y2 + 2) fit_fail (t, c.y2 - c.y1 + 1, pc.y2 - c.y1 + 1, "below its panel");
			}
		}
	}
	uint32_t n = lv_obj_get_child_count (o);
	for (uint32_t i = 0; i < n; i++) fit_walk (lv_obj_get_child (o, (int32_t) i));
}
static int text_shown (lv_obj_t *o, const char *needle)	// a shown label under o has needle in its text
{
	if (lv_obj_has_flag (o, LV_OBJ_FLAG_HIDDEN)) return 0;
	if (lv_obj_check_type (o, &lv_label_class) && lv_label_get_text (o) && strstr (lv_label_get_text (o), needle)) return 1;
	uint32_t n = lv_obj_get_child_count (o);
	for (uint32_t i = 0; i < n; i++) if (text_shown (lv_obj_get_child (o, (int32_t) i), needle)) return 1;
	return 0;
}
// every shown object of a page, for check_bounds.py: its path of child numbers, its box in screen pixels, clickable or not
static void bounds_walk (FILE *f, lv_obj_t *o, const char *path)
{
	if (lv_obj_has_flag (o, LV_OBJ_FLAG_HIDDEN)) return;
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int dot = sidecar_ui_dot (), ox, oy;
	sidecar_ui_origin (&ox, &oy);
	fprintf (f, "%s %d %d %d %d %d\n", path, ox + c.x1 * dot, oy + c.y1 * dot, ox + (c.x2 + 1) * dot - 1, oy + (c.y2 + 1) * dot - 1,
		 lv_obj_has_flag (o, LV_OBJ_FLAG_CLICKABLE) ? 1 : 0);
	uint32_t n = lv_obj_get_child_count (o);
	for (uint32_t i = 0; i < n; i++)
	{
		char q[160];
		snprintf (q, sizeof q, "%s.%u", path, (unsigned) i);
		bounds_walk (f, lv_obj_get_child (o, (int32_t) i), q);
	}
}
static void shot (const char *name)
{
	char p[400];
	snprintf (s_ShotName, sizeof s_ShotName, "%s", name);
	run (8);
	fit_walk (lv_screen_active ());
	fit_walk (lv_layer_top ());
	snprintf (p, sizeof p, "%s/%s-%s.png", s_Dir, s_Pre, name);
	write_png (p);
	snprintf (p, sizeof p, "%s/%s-%s.bounds", s_Dir, s_Pre, name);
	FILE *f = fopen (p, "w");
	if (!f) { perror (p); exit (1); }
	bounds_walk (f, lv_screen_active (), "s");
	bounds_walk (f, lv_layer_top (), "t");
	fclose (f);
}

static void drag (int x0, int y0, int x1, int y1, int steps, const char *name)	// UI coordinates
{
	const int my = MY ();
	y0 += my, y1 += my;
	s_Px = sx (x0); s_Py = sy (y0); s_Down = 1; run (2);
	for (int i = 1; i <= steps; i++) { s_Px = sx (x0 + (x1 - x0) * i / steps); s_Py = sy (y0 + (y1 - y0) * i / steps); run (1); }
	run (2);
	if (name) shot (name);
	s_Down = 0; run (3);
}

static void drag_raw (int x0, int y0, int x1, int y1, int steps, const char *name) { s_bRaw = 1; drag (x0, y0, x1, y1, steps, name); s_bRaw = 0; }

// the touch controls: the finger moved and not lifted (lift () lifts it), and a double tap. Raw: a control's own coordinates.
static void drag_hold_raw (int x0, int y0, int x1, int y1, int steps)
{
	s_Px = sx (x0); s_Py = sy (y0); s_Down = 1; run (2);
	for (int i = 1; i <= steps; i++) { s_Px = sx (x0 + (x1 - x0) * i / steps); s_Py = sy (y0 + (y1 - y0) * i / steps); run (1); }
	run (1);
}
static void lift (void) { s_Down = 0; run (3); }
static void double_tap_raw (int x, int y) { tap_raw (x, y); run (1); tap_raw (x, y); run (2); }

#define EXPECT(c) do { if (!(c)) { printf ("FAIL shot.c:%d [%s] %s\n", __LINE__, s_Pre, #c); s_nFail++; } } while (0)
#define EXPECT_CMD(s) do { if (strcmp (s_Last, s)) { printf ("FAIL shot.c:%d [%s] last command \"%s\", wanted \"%s\"\n", __LINE__, s_Pre, s_Last, s); s_nFail++; } } while (0)

// A touch-editable control (th_meter_edit; the page's _obj): a knob (a vertical drag, up is more) or a bar (along it, right is
// more) whose drag starts from `base`, whose double tap sends `def`, over 0..max, drawn as `played` when idle; fmt the console
// line with one %d. Checked:
//  - a tap is a tap (a press and a lift inside the dead zone): nothing is sent, two taps a second apart are two taps
//  - a drag sends live lines while the finger is down, the first from the dead zone's edge on (from the BASE: nothing jumps to the
//    finger), the last one at the lift carries the final value; every line is the control's own (a drag clicks nothing)
//  - a drag to the nearer end clamps (0 / max); at 10 ms steps at most one line every 30 ms
//  - while it is down and a moment after, a new view does not move the control; then it shows the played value again
//  - a double tap sends the default, once (def < 0: none, nothing is sent); wait: the held value stays until the engine's moves (VOICE)
static int edit_value (int base, int d, int span, int max)	// the widget's rule (th_meter_edit)
{
	const int dz = TH->touch / 4 > 0 ? TH->touch / 4 : 1, eff = d > dz ? d - dz : d < -dz ? d + dz : 0;
	long n = (long) eff * max;
	n += n < 0 ? -span / 2 : span / 2;
	int v = base + (int) (n / span);
	return v < 0 ? 0 : v > max ? max : v;
}
static int hist_all (int from, const char *fmt)		// every line since `from` has the form fmt (one %d)
{
	char a[64];
	snprintf (a, sizeof a, "%s", fmt);
	*strstr (a, "%d") = 0;
	for (int i = from; i < s_nCmds; i++)
	{
		int v;
		if (strncmp (s_Hist[i & 255], a, strlen (a)) || sscanf (s_Hist[i & 255] + strlen (a), "%d", &v) != 1) return 0;
	}
	return 1;
}
static int hist_value (int i, const char *fmt)		// the value of line i
{
	char a[64];
	int v = -1;
	snprintf (a, sizeof a, "%s", fmt);
	*strstr (a, "%d") = 0;
	sscanf (s_Hist[i & 255] + strlen (a), "%d", &v);
	return v;
}
static void edit_move (int knob, int hx, int hy, int d, int steps)	// the finger from the middle d further (knob: up)
{
	if (knob) drag_hold_raw (hx, hy, hx, hy - d, steps); else drag_hold_raw (hx, hy, hx + d, hy, steps);
}
static void edit_check_at (int line, void *obj, int knob, int base, int def, int max, int played, const char *fmt, const char *what, void (*push) (int), int wait)
{
	lv_obj_t *o = (lv_obj_t *) obj;
	char want[64];
	if (!o) { printf ("FAIL shot.c:%d [%s] %s: no control\n", line, s_Pre, what); s_nFail++; return; }
	if (!lv_obj_has_flag (o, LV_OBJ_FLAG_CLICKABLE)) { printf ("FAIL shot.c:%d [%s] %s: not touchable\n", line, s_Pre, what); s_nFail++; return; }
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	const int hx = (c.x1 + c.x2) / 2, hy = (c.y1 + c.y2) / 2, dz = TH->touch / 4 > 0 ? TH->touch / 4 : 1, span = th_meter_span (o);
#define ECHK(cond) do { if (!(cond)) { printf ("FAIL shot.c:%d [%s] %s: %s (last \"%s\")\n", line, s_Pre, what, #cond, s_Last); s_nFail++; } } while (0)
	const int settle = wait ? 60 : 14;						// (a VOICE knob's hold ends after 2 s at the latest)
	run (wait ? 60 : 12);
	int c0 = s_nCmds;
	tap_raw (hx, hy), run (12), tap_raw (hx, hy), run (12);			// two taps, a second apart
	ECHK (s_nCmds == c0);
	drag_hold_raw (hx, hy, hx + dz / 2, hy + dz / 2, 3), lift (), run (12);		// a press that stays inside the dead zone
	ECHK (s_nCmds == c0);
	// a drag of about 30 units
	const int dir = base + 30 > max ? -1 : 1, D = dz + span * 30 / max, steps = 8;
	c0 = s_nCmds;
	edit_move (knob, hx, hy, dir * D, steps);
	ECHK (s_nCmds > c0 && hist_all (c0, fmt));					// live lines, only the control's own
	int first = -1;									// the first line: from the dead zone's edge, from the base
	for (int i = 1; i <= steps && first < 0; i++)
	{
		int d = dir * D * i / steps, v = edit_value (base, d, span, max);
		if (v != base) first = v;
	}
	ECHK (s_nCmds > c0 && hist_value (c0, fmt) == first);
	ECHK (th_meter_get (o) == edit_value (base, dir * D * steps / steps, span, max));	// it shows its own value
	if (push) push (1);							// a view arrives meanwhile: the knob keeps its own value
	run (2);
	ECHK (th_meter_get (o) == edit_value (base, dir * D, span, max));
	int live = s_nCmds - c0;
	lift ();
	snprintf (want, sizeof want, fmt, edit_value (base, dir * D, span, max));
	ECHK (s_nCmds - c0 >= live + 1 && hist_all (c0, fmt));				// the final value at the lift, always
	ECHK (!strcmp (s_Last, want));
	ECHK (th_meter_editing (o));							// held a moment: the engine's view catches up
	if (wait)	// a value the engine learns by a round trip (VOICE): held until the engine's value moves, not for a moment
	{
		run (14);
		ECHK (th_meter_editing (o));
		if (push) push (0);							// the views as they were (one view that differs is no proof yet: an echo of the writes)
		run (2);
		run (settle);								// the hold's end: the Rytm's value again
		ECHK (!th_meter_editing (o) && th_meter_get (o) == played);
	}
	else
	{
		run (14);
		ECHK (!th_meter_editing (o));
		if (push) push (0);							// the views as they were
		run (2);
		ECHK (th_meter_get (o) == played);
	}
	// to the nearer end: clamped
	const int up = base >= max / 2, D2 = dz + span * ((up ? max - base : base) + 10) / max;
	c0 = s_nCmds;
	edit_move (knob, hx, hy, up ? D2 : -D2, steps);
	lift ();
	snprintf (want, sizeof want, fmt, up ? max : 0);
	ECHK (!strcmp (s_Last, want) && hist_all (c0, fmt));
	run (settle);
	// at 10 ms steps: at most one line every 30 ms (a drag of 60 steps = 600 ms: at most 21 lines, then the final one)
	s_nTickMs = 10;
	c0 = s_nCmds;
	drag_hold_raw (hx, hy, hx + (knob ? 0 : dir * D), hy - (knob ? dir * D : 0), 60);
	int n10 = s_nCmds - c0;
	lift ();
	s_nTickMs = 40;
	ECHK (n10 >= 2 && n10 <= 21 && s_nCmds - c0 == n10 + 1);
	run (settle);
	// a double tap: the default, once (def < 0: none, two taps send nothing)
	c0 = s_nCmds;
	double_tap_raw (hx, hy);
	if (def < 0) ECHK (s_nCmds == c0 && th_meter_get (o) == played);
	else
	{
		snprintf (want, sizeof want, fmt, def);
		ECHK (s_nCmds == c0 + 1 && !strcmp (s_Last, want));
		ECHK (th_meter_get (o) == def);
	}
	run (settle);
#undef ECHK
}
// a control that must not take a finger now (not clickable: a drag and a double tap fall through, nothing is sent)
static void edit_none_at (int line, void *obj, const char *what)
{
	lv_obj_t *o = (lv_obj_t *) obj;
	if (!o) { printf ("FAIL shot.c:%d [%s] %s: no control\n", line, s_Pre, what); s_nFail++; return; }
	if (lv_obj_has_flag (o, LV_OBJ_FLAG_CLICKABLE)) { printf ("FAIL shot.c:%d [%s] %s: touchable\n", line, s_Pre, what); s_nFail++; }
}
#define EDIT_CHECK(obj, knob, base, def, max, played, fmt, push) edit_check_at (__LINE__, obj, knob, base, def, max, played, fmt, #obj, push, 0)
#define EDIT_CHECK_W(obj, knob, base, def, max, played, fmt, push) edit_check_at (__LINE__, obj, knob, base, def, max, played, fmt, #obj, push, 1)
#define EDIT_NONE(obj) edit_none_at (__LINE__, obj, #obj)

// draw cost on the Mac (the Pi's flush budget, docs/UI-THEMES.md "Cost"): microseconds a redraw of area a (0: all),
// the dot expansion included (the flush)
static double now_us (void) { struct timespec t; clock_gettime (CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static double bench (const lv_area_t *a, int n)
{
	double t0 = now_us ();
	for (int i = 0; i < n; i++)
	{
		if (a) lv_obj_invalidate_area (lv_screen_active (), a); else lv_obj_invalidate (lv_screen_active ());
		lv_refr_now (NULL);
	}
	return (now_us () - t0) / n;
}

static sidecar_ui_status_t s_St;
static int s_nContrastSt = -1;		// 0.5.1: the status's ui_contrast (-1: not known)
// Viper's alert bars (REC, "1 thing to look at", "No Rytm link"): none in any other look; in Viper the bar covers the label's line with a pad,
// holds the text where it is really drawn (left, centre or right in its box; from LVGL's own letter positions) and is no wider than text + pads,
// inside its parent; the cached ext draw size takes in the pad ring (a partial redraw must invalidate it)
static void alert_bars (int w)
{
	lv_obj_t *o = (lv_obj_t *) sidecar_ui_home_obj (w);
	if (TH->f_pxs && w == 17)		// the header strip is sized for "H16  888.8  PAUS" and clips: no text it can show is wider
	{
		static const char *const t[] = {"H16  999.9  PAUS", "A04  121.5  PLAY", "A01  100.0  STOP", "G09  999.0  REC", "---  ---  ----", "D12  10.0  PLAY", "B16  222.2  PAUS", "C11  777.7  STOP"};
		for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) EXPECT (th_text_width (TH->f_pxs, t[i]) <= th_text_width (TH->f_pxs, "H16  888.8  PAUS"));
	}
	lv_area_t bar, c, pc;
	if (!o && w == 17) return;			// (a header without the Rytm text: the other looks' layouts)
	const int on = o && th_alert_bar (o, &bar);
	EXPECT (o && on == (s_nContrastSt == 3 && TH->id == THEME_SIDECAR && lv_label_get_text (o)[0]));
	if (!on) return;
	lv_obj_get_coords (o, &c);
	lv_point_t p0, p1;
	lv_label_get_letter_pos (o, 0, &p0);
	lv_label_get_letter_pos (o, (uint32_t) strlen (lv_label_get_text (o)), &p1);
	EXPECT (bar.y1 < c.y1 && bar.y2 > c.y2);
	EXPECT (bar.x1 < c.x1 + p0.x && bar.x2 >= c.x1 + p1.x - 1);
	EXPECT (bar.x2 - bar.x1 <= p1.x - p0.x + 3 * LU (5));
	lv_obj_get_coords (lv_obj_get_parent (o), &pc);
	EXPECT (bar.x2 <= pc.x2 && bar.x1 >= pc.x1);
	EXPECT (lv_obj_get_ext_draw_size (o) >= LU (5));
}

static void status_init (int style)
{
	static const char *const tracks[8] = {"PLAITS", "FM", "JUNO", "DSPS", "DAISY", "MD", "GRAN", "WAVE"};
	sidecar_ui_status_t s = {1, 123456, 0, {-1, 38, 61, 57}, {0}, "link: usb"};
	for (int i = 0; i < 8; i++) s.track[i] = tracks[i];
	s.notes = 1; s.out_tracks = 0; s.mac = 1; s.drone = -1; s.fifo = 96; s.web = 1; s.rec = 2; s.rec_secs = 83;
	strcpy (s.fx_type[0], "CHORUS"); strcpy (s.fx_type[1], "COMB"); s.fx_post[0] = 1;
	s.fx_bw[0][0] = 24, s.fx_bw[0][1] = 70, s.fx_bw[1][0] = 0, s.fx_bw[1][1] = 127;
	for (int t = 0; t < SIDECAR_UI_SENDS; t++) { s.send[0][t] = (t * 11) % 128; s.send[1][t] = 127 - (t * 9) % 128; }
	strcpy (s.ip, "169.254.61.60"); strcpy (s.host, "sidecar.local"); strcpy (s.link, "usb"); strcpy (s.gadget, "sidecar");
	strcpy (s.build, "0.2.0 (oled) Oct 20 2026"); strcpy (s.circle, "Circle 51.1.1");
	strcpy (s.project, "SHARED SET");
	static const char *const us[5] = {"ACID", "GRAIN", "!MISS WOBB", "METAL...", "---"};	// (um.cpp ui_view: "!" + rmp_why_name)
	static const unsigned char ust[5] = {1, 1, 2, 6, 0};
	for (int i = 0; i < 5; i++) { strcpy (s.uslot[i], us[i]); s.uslot_state[i] = ust[i]; }
	s.screensaver = 60; s.brightness = 100; s.touch_hidden = 0; s.cpu_mhz = 1500;
	s.usb_mode = 0; s.usb_modes = 3; s.usb_shown = 0;	// the USB-C mode: CLASS COMPLIANT, both modes built
	s.keys_mode = 0; s.keys_auto = 14; s.keys_mac = 1;	// Settings > MIDI: SELECTED, auto channel 14, computer notes on
	strcpy (s.keys_in, "MPKMINI, LAUNCHCONTROLXL"); strcpy (s.keys_last, "MPKMINI > SD POLY"); s.learn_routes = 2;
	s.ui_style = style;
	s.ui_look = -1;
	s.ui_contrast = s_nContrastSt;
	s.poly = 1u << 7;		// RS/CP (voice 7, WAVE): a poly track
	s.pattern = 3, s.transport = 1, s.bpm10 = 1215;	// A04 playing (HOME's NOW)
	{ static const int vl[8] = {38, 64, 140, 120, 22, 57, 93, 210}; for (int v = 0; v < 8; v++) s.vload[v] = vl[v]; }	// HOME's voice tiles: tenths of a percent
	s_St = s;
}

// ---- the machine view's made-up state (the Pi fills it from core/granular, core/wavetable: wgview_pi.cpp): GRANULAR on
// a bells-like source with a cloud around a head that ANIM moves, or WAVETABLE morphing sine -> saw -> square
static sidecar_wg_view_t s_Wg;
static int s_WgSelB = -1, s_WgMix0;		// TBLB: the code table B is set to, and B silent for lack of MIX (wgview_pi.cpp's sel_b / mix0)
// rui1036b: the VOICE page's view, faked: an analog BD (BD PLASTIC: its SRC labels, a kick on the scope, locks on a
// few steps of a 16-step pattern) or a Sidecar track (MULTISAMPLE / 6-OP FM)
static sidecar_voice_view_t s_Vv;
// the MASTER FX page's view, as insfx_pi.cpp's FillMaster fills it
static sidecar_mfx_view_t s_Mv;
// 0.5.0: the MORPH page's made-up scenes (A 1, B 3, scene 3 selected with n locks, the longest names and values)
static sidecar_morph_view_t s_Mo;
static void morph_fake (int n, int top)
{
	static const char *const kN[] = {"V1:P3", "RT12:FTYP", "MFX:LEV", "I8:A4", "RFX:RDEC", "X8:B3", "O8:WID", "V8:P16"};
	static const char *const kL[] = {"V1:TIM", "RT12:FTYP", "MFX:LEV", "I8:ASHPE", "RFX:RDEC", "X8:B3", "O8:WID", "V8:SEED"};	// as morph_pi labels them
	static const char *const kV[] = {"64", "7", "100.5", "127.9", "12", "0", "64", "127"};
	memset (&s_Mo, 0, sizeof s_Mo);
	s_Mo.valid = 1, s_Mo.fader = 9000, s_Mo.a = 0, s_Mo.b = 2, s_Mo.sel = 2, s_Mo.top = (unsigned char) top;
	for (int k = 0; k < SMO_SCENES; k++) s_Mo.nlocks[k] = (unsigned char) (k == 2 ? n : k == 0 ? 5 : k == 11 ? 64 : 0);
	for (int i = top; i < n && s_Mo.nrows < SMO_ROWS; i++)
	{
		sidecar_morph_row_t *r = &s_Mo.row[s_Mo.nrows++];
		snprintf (r->name, sizeof r->name, "%s", kN[i % 8]);
		snprintf (r->label, sizeof r->label, "%s", kL[i % 8]);
		snprintf (r->value, sizeof r->value, "%s", kV[i % 8]);
		r->lo = 0, r->hi = i % 8 == 1 ? 7 : 127, r->v = (unsigned char) (i * 13 % 128);
	}
	s_Mo.gen = (unsigned) (n * 1000 + top + 1);
}
// 0.5.0: the LOOP page's made-up state (n grabs, a loop playing, the longest texts), as loopview_pi.cpp fills it
static sidecar_loop_view_t s_Lo;
static void loop_fake (int n, int state)
{
	static const unsigned char kHb[8] = {2, 1, 32, 8, 4, 16, 2, 1};
	memset (&s_Lo, 0, sizeof s_Lo);
	s_Lo.valid = 1, s_Lo.on = 1, s_Lo.state = (unsigned char) state, s_Lo.ring_s = 87, s_Lo.bal = 64, s_Lo.lev = 100, s_Lo.fade = 2;
	s_Lo.mute = 1, s_Lo.live = 12, s_Lo.playing = n ? 0 : -1, s_Lo.sel = 0, s_Lo.ngrabs = (unsigned char) n;
	if (state) s_Lo.pos10 = 27, s_Lo.bars10 = 40;
	for (int i = 0; i < n; i++)
	{
		s_Lo.g[i].hb = kHb[i], s_Lo.g[i].kept = i == 1, s_Lo.g[i].gap = i == 3, s_Lo.g[i].copied = 1, s_Lo.g[i].bpm10 = (unsigned short) (1200 + i * 87), s_Lo.g[i].id = (unsigned) (100 + i);
	}
	if (n) snprintf (s_Lo.msg, sizeof s_Lo.msg, "KEPT SIDECAR_2026-11-13_1200");
	s_Lo.gen = (unsigned) (n * 10 + state + 1);
}
// 0.5.0: the XY page's made-up setup (the longest names, both signs, MORPH), HOLD and a held position as asked
static sidecar_xy_view_t s_Xy;
static void xy_fake (int slots, int hold, int active)
{
	static const char *const kN[2][SXY_SLOTS] = {{"RYTM:PERF12", "MORPH", "I8:B4", "V8:P16"}, {"MFX:MIX4", "RYTM:PERF1", "T12:RWGT", "X8:B3"}};
	// as xypad_pi labels them: V8 plays DAISYSP (P16 WID), I8's slot B a DRIVE (P4 MIX)
	static const char *const kL[2][SXY_SLOTS] = {{"RYTM:PERF12", "MORPH", "I8:BMIX", "V8:WID"}, {"MFX:MIX4", "RYTM:PERF1", "T12:RWGT", "X8:B3"}};
	static const char *const kPlaits[16] = {"ENG", "HRM", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "FM", "FIN", "AUX", "TMD", "MMD", "VEL", "GLD", "VOL"};
	static const char *const kDaisy[16] = {"ENG", "STR", "BRI", "DMP", "INP", "EXC", "TUN", "LEV", "XTN", "XDC", "SEED", "OUT", "XP4", "XLV", "XP5", "WID"};
	static const char *const kDrive[4] = {"DRV", "SHPE", "TONE", "MIX"};
	static const signed char kD[2][SXY_SLOTS] = {{100, -50, 25, -100}, {-75, 100, 5, -5}};
	memset (&s_Xy, 0, sizeof s_Xy);
	s_Xy.valid = 1, s_Xy.hold = (unsigned char) hold, s_Xy.active = (unsigned char) active, s_Xy.x = 11000, s_Xy.y = 4000;
	for (int a = 0; a < 2; a++)
		for (int k = 0; k < SXY_SLOTS && k < slots; k++)
			strcpy (s_Xy.s[a][k].name, kN[a][k]), strcpy (s_Xy.s[a][k].label, kL[a][k]), s_Xy.s[a][k].depth = kD[a][k];
	for (int v = 0; v < 8; v++) for (int p = 0; p < 16; p++) strcpy (s_Xy.vl[v][p], v == 7 ? kDaisy[p] : kPlaits[p]);
	for (int p = 0; p < 4; p++) strcpy (s_Xy.fl[0][p], kDrive[p]), strcpy (s_Xy.fl[1][p], "-");
	for (int c = 0; c < 13; c++)
		for (int p = 0; p < 12; p++)
		{
			static const char *const kEq[4] = {"QHPF", "QLPF", "QLOW", "QHIG"};
			if (p >= 8) strcpy (s_Xy.il[c][p], kEq[p - 8]);
			else if (c == 7 && p >= 4) snprintf (s_Xy.il[c][p], sizeof s_Xy.il[c][p], "B%s", kDrive[p - 4]);
			else snprintf (s_Xy.il[c][p], sizeof s_Xy.il[c][p], "%c-", 'A' + p / 4);
		}
	s_Xy.gen = (unsigned) (slots * 100 + hold * 10 + active + 1);
}
static void mfx_slot (int i, int type, const char *n, const char *next, const char *l0, const char *v0, int send, int amp)
{
	sidecar_mfx_slot_t *x = &s_Mv.slot[i];
	static const char *const kL[3] = {"RES", "MODE", "DRV"}, *const kV[3] = {"7.2:1", "+6DB", "30"};
	x->type = type, strcpy (x->name, n), strcpy (x->next, next), x->send = send, x->amp = amp, x->mix = 64, x->pair = amp;
	strcpy (x->label[0], l0), strcpy (x->val[0], v0);
	for (int k = 1; k < 4; k++) strcpy (x->label[k], kL[k - 1]), strcpy (x->val[k], kV[k - 1]);
	for (int k = 0; k < 4; k++) x->v[k] = 30 + 25 * k, x->base[k] = x->v[k], x->def[k] = (unsigned char) (40 + 10 * k);
	x->mixdef = 40;
}
static void mfx_fake (int src)
{
	memset (&s_Mv, 0, sizeof s_Mv);
	s_Mv.valid = 1, s_Mv.on = 1, s_Mv.mix = src, s_Mv.lev = 100, s_Mv.lev_def = 100, s_Mv.in = 70, s_Mv.out = 90, s_Mv.gen = 100 + src;
	mfx_slot (0, 5, "COMP", "DELAY", "THR", "-16DB", 0, 0);
	mfx_slot (1, 9, "AMP", "CHORUS", "MODL", "PLEXICR", 0, 1);
	mfx_slot (2, 12, "SPACE", "RINGS", "TIME", "1/8", 1, 0);
	strcpy (s_Mv.slot[3].name, "OFF"), strcpy (s_Mv.slot[3].next, "FILTER");
	strcpy (s_Mv.peq, "HPF 40  LPF OFF  LOW FLAT  HIGH +2.4DB");
}
// fx-ins: the INSERT FX page's view, faked as insfx_pi.cpp fills it: voice 3 (BT, a WAVETABLE chord) through FILTER +
// DELAY, sends to bus 1 (CLOUDSEED + EQ) and bus 3 (dry, COMP, POST)
static sidecar_insfx_view_t s_Iv;
// touch2: SCOPE's view as scope_pi.cpp fills it (the tap's columns for the page's source and timebase, the recorder's
// meters): BD a decaying kick, MAIN a detuned chord, the others a quieter tone; the meters a beat's levels
static sidecar_scope_view_t s_Sc;
static void scope_fake (int frame)
{
	sidecar_scope_view_t *v = &s_Sc;
	static const char *const kN[SSC_SOURCES] = {"MAIN", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
	memset (v, 0, sizeof *v);
	v->valid = 1, v->track = 0;
	int src = sidecar_scope_source () < 0 ? 1 : sidecar_scope_source ();
	v->src = src, v->tb = sidecar_scope_timebase ();
	for (int k = 0; k < SSC_SOURCES; k++) snprintf (v->name[k], sizeof v->name[k], "%s", kN[k]);
	int e = sidecar_scope_tb_entries (v->tb);
	v->ncols = SSC_COLS;
	v->cursor = v->tb >= SSC_TRIG_TBS ? (frame * 23 + 140) % SSC_COLS : -1;
	for (int i = 0; i < SSC_COLS; i++)
	{
		double lo = 1, hi = -1;
		for (int f = 0; f < e * 8; f += (e > 4 ? e : 1))
		{
			double t = (double) (i * e * 8 + f) / 48000.0 + frame * 0.0007, y;
			if (src == 1) { double tk = fmod (t, 0.5); y = 0.85 * exp (-tk * 9) * sin (2 * M_PI * (48 + 160 * exp (-tk * 40)) * tk); }
			else if (src == 0) y = 0.28 * (sin (2 * M_PI * 110 * t) + 0.7 * sin (2 * M_PI * 164.8 * t + 0.3) + 0.5 * sin (2 * M_PI * 220.6 * t));
			else y = 0.12 * sin (2 * M_PI * (180 + 40 * src) * t) * (0.6 + 0.4 * sin (2 * M_PI * 3 * t));
			if (y < lo) lo = y;
			if (y > hi) hi = y;
		}
		v->lo[i] = (short) (lo * 32767), v->hi[i] = (short) (hi * 32767);
	}
	static const float kLev[SSC_CH] = {0.55f, 0.5f, 0.92f, 0.3f, 0.12f, 0.4f, 0.05f, 0.0f, 0.22f, 0.08f, 0.6f, 0.58f};
	for (int c = 0; c < SSC_CH; c++)
	{
		float l = kLev[c] * (0.75f + 0.25f * (float) sin (frame * 0.7 + c));
		v->peak[c] = (unsigned) (l * l * SSC_FS);
	}
	v->peak[2] = frame % 4 == 0 ? SSC_FS : v->peak[2];	// BD clips on the beat
	v->gen = (unsigned) frame * 7 + (unsigned) src * 1000 + (unsigned) v->tb * 100000 + 1;
}
// touch2: SIDECAR STATUS's view as kernel.cpp fills it once a second
static sidecar_status_view_t s_Stv;
static int s_FakeMHz = 1500;
static int s_FakeTemp = 58, s_FakeWarn = 0, s_FakeLow = 0;	// the STATUS page's temperature (thermal.h: warned from 80 C, until below 75 C), the clock the heat lowered
static void status_fake (int alerts)
{
	sidecar_status_view_t *v = &s_Stv;
	static const char *const m[8] = {"PLAITS", "FM", "JUNO", "DSPS", "DAISY", "MULTI", "GRAN", "WAVE"};
	static const int ld[8] = {38, 121, 64, 22, 93, 57, 208, 144};
	memset (v, 0, sizeof *v);
	v->valid = 1;
	for (int i = 0; i < 8; i++) { snprintf (v->mach[i], sizeof v->mach[i], "%s", m[i]); v->vload[i] = ld[i]; }
	v->load[0] = -1, v->load[1] = 38, v->load[2] = 61, v->load[3] = 57;
	v->block_max_us = 182, v->block_budget_us = 250, v->pass_max_us = 410, v->underruns = 0, v->fifo_min = 61, v->fifo_target = 96;
	v->temp_c = s_FakeTemp, v->temp_warn = s_FakeWarn, v->cpu_mhz = s_FakeLow ? s_FakeLow : s_FakeMHz, v->clock_low_mhz = s_FakeLow;
	snprintf (v->mach[0], sizeof v->mach[0], "PLAITS x8"), snprintf (v->mach[4], sizeof v->mach[4], "DAISY x4");	// E0
	v->inst_used = 12, v->inst_total = 32, v->inst_cpu100 = 61, v->inst_kb = 35;
	strcpy (v->fx_type[0], "CHORUS"), strcpy (v->fx_type[1], "COMB"), v->fx_post[0] = 1;
	v->rytm = 1, v->ob = 1, v->blocks = 1234567;
	strcpy (v->link, "usb"), strcpy (v->gadget, "sidecar"), v->mac = 1;
	v->net = 2, v->wifi = 1;
	strcpy (v->ip, "192.168.1.42"), strcpy (v->host, "sidecar.local");
	strcpy (v->pair, "1234 5678");				// the pairing code (pi/engine/paircode.h)
	v->card_free_mb = 24310, v->card_total_mb = 30436, v->ui_max_us = 6800;
	strcpy (v->build, "0.3.0 (touch2) Oct 21 2026"), strcpy (v->circle, "Circle 51.1.1");
	strcpy (v->rytm_mod, "RUI1036+ (SEEN)");
	v->usb_state = 2, v->usb_free_mb = 58982, v->usb_total_mb = 61056;	// a 64 GB stick on a blue port, an NVMe SSD
	strcpy (v->usb_fs, "exFAT"), strcpy (v->usb_link, "USB 3");
	v->nvme_state = 2, v->nvme_free_mb = 230400, v->nvme_total_mb = 244198, strcpy (v->nvme_fs, "FAT32");
	v->lib_state = 3, v->lib_files = 48213, v->lib_hashed = 29410, v->lib_pending = 18803, v->lib_pct = 61;	// the library hashing
	v->lib_dups = 212, strcpy (v->lib_waiting, "PLAYING");
	if (alerts)						// a muted voice, underruns, no network, a full card, no mod seen
	{
		v->usb_state = 1, v->nvme_state = 4;			// a stick that does not mount; a Pi 4
		v->muted[6] = 1, v->vload[6] = 1000, v->underruns = 17, v->net = 1, v->wifi = 0, v->card_free_mb = 612;
		v->load[2] = 93, v->pass_max_us = 2600, v->ob = 0, v->rytm_mod[0] = 0, v->ui_max_us = 17400;
		v->lib_state = 4, v->lib_pct = 100, v->lib_pending = 0, v->lib_missing = 7, v->lib_waiting[0] = 0;	// ready, files missing
		strcpy (v->pair, "OFF (cmdline)"), v->pair_warn = 1;	// the alerts shot: a dev card's gate off
		v->inst_used = 32;					// E0: the instance pool full
	}
	v->gen = 4242u + (unsigned) alerts + 10u * (unsigned) s_FakeTemp + (unsigned) s_FakeWarn + 100000u * (unsigned) s_FakeMHz + 7u * (unsigned) s_FakeLow;
}
static void ins_slot (int c, int s, int type, const char *name, const char *l0, const char *l1, const char *l2, const char *l3,
		      const char *v0, const char *v1, const char *v2, const char *v3, int a, int b, int cc, int d)
{
	sidecar_insfx_slot_t *x = &s_Iv.slot[c][s];
	x->type = type;
	snprintf (x->name, sizeof x->name, "%s", name);
	const char *l[4] = {l0, l1, l2, l3}, *v[4] = {v0, v1, v2, v3};
	int n[4] = {a, b, cc, d};
	for (int k = 0; k < 4; k++)
		snprintf (x->label[k], 5, "%s", l[k]), snprintf (x->val[k], 8, "%s", v[k]), x->v[k] = (unsigned char) n[k],
		x->base[k] = (unsigned char) (n[k] > 120 ? n[k] - 3 : n[k] + 3), x->def[k] = (unsigned char) (50 + 5 * k);	// (a base that is not the played value: an LFO / a lock)
}
static void ins_fake (unsigned gen)
{
	memset (&s_Iv, 0, sizeof s_Iv);
	s_Iv.valid = 1, s_Iv.voice = 2, s_Iv.track = 4;
	ins_slot (2, 0, 1, "FILTER", "CUT", "RES", "MODE", "DRV", "1.6K", "20", "LP>BP", "+6DB", 80, 20, 30, 32);
	ins_slot (2, 1, 6, "DELAY", "TIME", "FDBK", "MOD", "MIX", "22MS", "40", "30", "64", 90, 40, 30, 64);
	ins_slot (8, 0, 4, "EQ", "LOW", "MID", "HIGH", "FREQ", "+3.5DB", "0DB", "-2.3DB", "1.2K", 79, 64, 54, 64);
	ins_slot (10, 0, 5, "COMP", "THR", "RAT", "SPD", "GAIN", "-16DB", "3.1:1", "40", "+4DB", 50, 60, 40, 20);
	s_Iv.send[2][0] = 90, s_Iv.send[2][2] = 64;
	snprintf (s_Iv.bus_fx[0], 12, "CLOUDSEED"), snprintf (s_Iv.bus_fx[1], 12, "OFF");
	s_Iv.bus_post[2] = 1;
	for (int v = 0; v < 8; v++)	// the OUT route: every voice TRK, LEV 100 PAN C WID 64; voice 3 POST (LEV 90, PAN L20, WID 100)
	{
		s_Iv.out_v[v][0] = 100, s_Iv.out_v[v][1] = 64, s_Iv.out_v[v][2] = 64;
		snprintf (s_Iv.out_val[v][0], 6, "100"), snprintf (s_Iv.out_val[v][1], 6, "C"), snprintf (s_Iv.out_val[v][2], 6, "64");
	}
	s_Iv.out_def[0] = 100, s_Iv.out_def[1] = 64, s_Iv.out_def[2] = 64;
	for (int v = 0; v < 8; v++) for (int k = 0; k < 3; k++) s_Iv.out_base[v][k] = s_Iv.out_v[v][k];
	s_Iv.out_route[2] = 2, s_Iv.out_v[2][0] = 90, s_Iv.out_v[2][1] = 44, s_Iv.out_v[2][2] = 100;
	s_Iv.out_base[2][0] = 80, s_Iv.out_base[2][1] = 40, s_Iv.out_base[2][2] = 100;	// (as set: offsets on the played values)
	snprintf (s_Iv.out_val[2][0], 6, "90"), snprintf (s_Iv.out_val[2][1], 6, "L20"), snprintf (s_Iv.out_val[2][2], 6, "100");
	s_Iv.out_peak[2][0] = 96, s_Iv.out_peak[2][1] = 71;
	// ins1112: the voice's POST EQ (the hint line) and bus 1's (the bus line's "> EQ"), as insfx_pi.cpp fills them
	snprintf (s_Iv.peq[2], sizeof s_Iv.peq[2], "HPF 420  LPF 6.2K  LOW +3.0DB  HIGH FLAT");
	snprintf (s_Iv.peq[8], sizeof s_Iv.peq[8], "HPF OFF  LPF 1.9K  LOW FLAT  HIGH -4.3DB");
	s_Iv.gen = gen;
}
// the touch edits' check pushes a view while a finger is down: every played value of the page changed (on), or as it was (off)
static unsigned s_nPushGen = 7000;
static void push_ins (int on)
{
	ins_fake (++s_nPushGen);
	if (on)
	{
		for (int c = 0; c < SIF_CHAINS; c++) for (int t = 0; t < 2; t++) for (int k = 0; k < 4; k++) s_Iv.slot[c][t].v[k] = 3;
		for (int v = 0; v < 8; v++) for (int b = 0; b < 3; b++) s_Iv.send[v][b] = 3, s_Iv.out_v[v][b] = 3;
	}
	sidecar_insfx_set (&s_Iv);
}
static void push_mfx (int on)
{
	mfx_fake (0);
	s_Mv.gen = ++s_nPushGen;
	if (on)
	{
		s_Mv.lev = 3;
		for (int i = 0; i < SMF_SLOTS; i++) { s_Mv.slot[i].mix = 3; for (int k = 0; k < 4; k++) s_Mv.slot[i].v[k] = 3; }
	}
	sidecar_mfx_set (&s_Mv);
}
static void voice_fake (int kind);
static int s_nVoiceKind = 2;
static void push_voice (int on)		// the Rytm's own values changed (on: every knob 3), or as they were
{
	voice_fake (s_nVoiceKind);
	if (on) for (int k = 0; k < 8; k++) s_Vv.val[k] = 3;
	s_Vv.gen = ++s_nPushGen;
	sidecar_voice_set (&s_Vv);
}
static void voice_fake (int kind)
{
	sidecar_voice_view_t *v = &s_Vv;
	memset (v, 0, sizeof *v);
	// kind 4: SAMPLER (0.5.0; voice_pi: its page's knobs, SMP STRT END PLAY / TUNE FINE LOOP LEV)
	static const char *kLa[5][8] = {{"LEV", "TUN", "DEC", "TYP", "MOD", "SWT", "SWD", "TIC"},
					{"INST", "VEL", "LEV", "RTRG", "ATK", "DEC", "SUS", "REL"},
					{"PAT", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "LEN"},
					{"ENG", "STR", "BRI", "DMP", "INP", "EXC", "TUN", "LEV"},
					{"SMP", "STRT", "END", "PLAY", "TUNE", "FINE", "LOOP", "LEV"}};
	static const unsigned char kVa[5][8] = {{100, 56, 97, 0, 27, 99, 127, 127}, {3, 127, 100, 0, 20, 70, 90, 40},
						{10, 64, 64, 127, 64, 64, 100, 127}, {10, 70, 90, 60, 0, 64, 64, 100},
						{1, 0, 120, 15, 64, 64, 0, 100}};
	static const char *kName[5] = {"BD PLASTIC", "MULTISAMPLE", "6-OP FM", "DAISYSP", "SAMPLER"};	// (voice_pi: a Sidecar machine's long name)
	v->valid = 1, v->track = kind == 1 ? 5 : kind == 2 ? 2 : kind == 3 ? 8 : kind == 4 ? 4 : 0;
	v->machine = kind == 1 ? 52 : kind == 2 ? 40 : kind == 3 ? 51 : kind == 4 ? 43 : 21;
	v->mode = kind == 1 ? SVV_MULTI : kind == 2 ? SVV_FM : kind == 3 ? SVV_DAISY : SVV_PLAIN;
	if (kind == 2)		// E.PIANO-like: algorithm 5 (three stacks of two, op 6's feedback), as fm_info gives it
	{
		static const signed char col[6] = {0, 0, 1, 1, 2, 2}, lev[6] = {0, 1, 0, 1, 0, 1};
		static const unsigned char mod[6] = {2, 0, 8, 0, 32, 0}, out[6] = {99, 58, 99, 89, 99, 79};
		static const char *fr[6] = {"1.0", "14", "1.0", "1.0", "1.0", "1.0"};
		snprintf (v->fm_name, sizeof v->fm_name, "E.PIANO 1");
		v->fm_alg = 4, v->fm_fb = 6, v->fm_ncols = 3, v->fm_nlev = 2, v->fm_car = 21, v->fm_fb_src = 5, v->fm_fb_dst = 5;
		for (int k = 0; k < 6; k++)
		{
			v->fm_col[k] = col[k], v->fm_lev[k] = lev[k], v->fm_mod[k] = mod[k], v->fm_out[k] = out[k];
			snprintf (v->fm_freq[k], 6, "%s", fr[k]);
		}
	}
	if (kind == 1)		// a piano in 5 zones of 2 layers over C1..C6, a chord sounding
	{
		snprintf (v->ms_inst, sizeof v->ms_inst, "UPRIGHT PIANO");
		v->ms_ready = 1, v->ms_nz = 6;
		static const unsigned char lo[6] = {24, 36, 48, 55, 67, 79}, hi[6] = {35, 47, 54, 66, 78, 96};
		for (int z = 0; z < 6; z++) v->ms_lo[z] = lo[z], v->ms_hi[z] = hi[z], v->ms_root[z] = (unsigned char) ((lo[z] + hi[z]) / 2), v->ms_vel[z] = 2;
		static const unsigned char keys[4] = {48, 52, 55, 60};
		for (int i = 0; i < 4; i++) v->ms_keys[keys[i] >> 3] |= (unsigned char) (1u << (keys[i] & 7));
	}
	if (kind == 3)		// MODAL: a bar-like stretch (as voice_pi's fill_daisy for STR 70 BRI 90 DMP 60)
	{
		v->dz_model = 0, v->dz_n = SVV_MODES;
		double f = 1.0, st = 0.4;
		for (int i = 0; i < SVV_MODES; i++)
		{
			v->dz_ratio[i] = (unsigned short) (f * 100);
			v->dz_ring[i] = (unsigned char) (230 * pow (0.86, i));
			f += 1.0 + st * i, st *= 0.98;
		}
	}
	v->sidecar = kind != 0;
	snprintf (v->name, sizeof v->name, "%s", kName[kind]);
	for (int k = 0; k < 8; k++) { snprintf (v->label[k], 5, "%s", kLa[kind][k]); v->val[k] = kVa[kind][k]; }
	// the touch edits (voice_pi): a Sidecar track's knobs but the list / banked P1 (INST, PAT, ENG, SAMPLER's SMP); SAMPLER's page maps its knobs
	static const unsigned char kSmplK[8] = {10, 12, 13, 11, 8, 9, 14, 15};
	for (int k = 0; k < 8; k++) v->wk[k] = kind == 4 ? kSmplK[k] : (unsigned char) k;
	v->wedit = kind ? 0xFE : 0;
	v->ncols = SVV_COLS;
	for (int i = 0; i < SVV_COLS; i++)	// a kick (or a pad) decaying over the columns
	{
		double ph = fmod (i * (kind ? 1.0 : 2.0) / SVV_COLS, 1.0), e = exp (-ph * (kind ? 1.2 : 6.0));
		double w = sin (i * (kind ? 1.7 : 0.55));
		v->smax[i] = (signed char) (110 * e * (0.6 + 0.4 * fabs (w))), v->smin[i] = (signed char) (-104 * e * (0.6 + 0.4 * fabs (w)));
	}
	v->level = kind ? 74 : 101;
	v->steps = kind == 1 ? 32 : 16, v->pattern = 3;
	for (int s2 = 0; s2 < v->steps; s2++) v->trig[s2] = kind ? (s2 % 3 == 0) : (s2 % 4 == 0);
	v->locks[2] = 1, v->locks[4] = 3, v->locks[8] = 1, v->locks[11] = 6, v->locks[12] = 2;
	if (kind == 1) v->locks[20] = 4, v->locks[27] = 1;
	v->gen = 100 + (unsigned) kind;
}

// the three POOL lists, made up (the Pi reads core/lib/pools.h): one row's entry, its name, state and voices on it
static int s_WgPlaying, s_WgRoot, s_WgRamKnown = 1;
static unsigned s_nWgGen;
static int wg_row (int l, int b, int r, const char **name, int *flags, int *users)
{
	static const char *const kIn[12] = {"IN", "MAIN", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB", "PAD", "BELLS"};
	static const char *const kTbl[7] = {"BASIC", "PWM", "VOCAL", "ORGAN", "FM", "SYNC", "glass pad"};
	*name = "", *flags = 0, *users = 0;
	if (r == 0) { *name = "OFF"; return WGV_RT_OFF; }
	if (l == WGV_SAMPLES && b == 0 && r <= 10 && (r < 7 || r == 10))
	{
		static const char *const kA[11] = {"", "kick 909", "snare tight", "hat closed", "choir aah", "vinyl crackle", "rain", "", "", "", "breakbeat"};
		static const unsigned char kF[11] = {0, WGV_RF_RAM | WGV_RF_USED, WGV_RF_RAM | WGV_RF_USED, WGV_RF_LOADING, WGV_RF_MISSING | WGV_RF_USED,
						     WGV_RF_CHANGED, 0, 0, 0, 0, WGV_RF_RAM | WGV_RF_USED};
		static const unsigned char kU[11] = {0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 1};
		*name = kA[r], *flags = kF[r], *users = kU[r];
		return WGV_RT_SAMPLE;
	}
	if (l == WGV_SAMPLES && b == 2 && r <= 3)
	{
		static const char *const kC[4] = {"", "pad warm", "pad glass", "drone low"};
		*name = kC[r], *flags = r == 1 ? WGV_RF_RAM | WGV_RF_USED : r == 3 ? WGV_RF_USED : 0, *users = r == 1 ? 3 : 0;
		return WGV_RT_SAMPLE;
	}
	if (l == WGV_SAMPLES && b == 7 && r <= 12)
	{
		*name = kIn[r - 1], *flags = WGV_RF_RAM | (r == 1 ? WGV_RF_USED : 0);
		return r <= 10 ? WGV_RT_INPUT : WGV_RT_BUILTIN;
	}
	if (l == WGV_TABLES && b == 0 && r <= 7)
	{
		*name = kTbl[r - 1], *flags = r == 7 ? WGV_RF_LOADING : WGV_RF_RAM | (r == 1 ? WGV_RF_USED : 0), *users = r == 1;
		return r <= 6 ? WGV_RT_BUILTIN : WGV_RT_SAMPLE;
	}
	if (l == WGV_INSTR && (b == 0 || b == 3) && r <= 3)
	{
		static const char *const kI[2][4] = {{"", "UPRIGHT PIANO", "STRINGS SECT", "Rhodes Mk1"}, {"", "Choir Aah", "Marimba", "Wurli 200"}};
		*name = kI[b == 3][r];
		*flags = b == 0 ? (r == 1 ? WGV_RF_RAM | WGV_RF_USED : r == 2 ? WGV_RF_LOADING : WGV_RF_MISSING) : 0;
		*users = b == 0 && r == 1;
		return WGV_RT_INSTR;
	}
	return WGV_RT_EMPTY;
}

static void wg_fake (int kind, double t)
{
	sidecar_wg_view_t *v = &s_Wg;
	memset (v, 0, sizeof *v);
	v->kind = kind, v->voice = 2, v->track = 2, v->ncols = WGV_COLS;
	v->gen = ++s_nWgGen;
	// the POOL: the bank of the list the view shows (sidecar_wgview_list_id, _bank), the RAM, the used set
	int l = sidecar_wgview_list_id (), b = sidecar_wgview_bank ();
	v->list = l, v->bank = b, v->sel = -1, v->playing = s_WgPlaying;
	v->sel_b = s_WgSelB, v->mix0 = s_WgMix0;
	if (s_WgSelB >= 0) snprintf (v->name_b, sizeof v->name_b, "%s", s_WgSelB ? "BASIC" : "OFF");
	v->count[0] = 22, v->count[1] = 8, v->count[2] = 6;
	v->ram_known = s_WgRamKnown, v->ram_mb = 412, v->ram_budget_mb = 1024, v->ram_pct = 40;
	snprintf (v->warn, sizeof v->warn, "NOT SEEN PLAYING - A RYTM LOCK MAY STILL USE IT");
	for (int r = 0; l >= 0 && r < WGV_ROWS; r++)
	{
		const char *nm;
		int f, u;
		v->rtype[r] = (unsigned char) wg_row (l, b, r, &nm, &f, &u);
		snprintf (v->rname[r], WGV_NAME, "%s", nm);
		v->rflags[r] = (unsigned char) f, v->rusers[r] = (unsigned char) u;
		if (v->rtype[r] == WGV_RT_SAMPLE && !(f & WGV_RF_MISSING))	// docs/STEREO.md: the channels' tags (every third mono)
			v->rflags[r] |= r % 3 ? WGV_RF_ST : WGV_RF_MONO;
	}
	if (l >= 0 && sidecar_wgview_selecting ())	// SELECT UNUSED: the filled rows not seen playing
		for (int c = 0; c < WGV_CODES; c++)
		{
			const char *nm;
			int f, u, ty = wg_row (l, c >> 7, c & 127, &nm, &f, &u);
			if (ty >= WGV_RT_SAMPLE && !(f & WGV_RF_USED)) v->unused[v->nunused++] = (short) c;
		}
	// the browser (core/lib/srclib.h) of the list's machine: /Samples/Drums - two folders, then its files (one in the
	// list already: C001), or the root with the virtual folders the list takes
	static const char *kLib[] = {"Kicks", "Snares", "808 kick.wav", "clap.aif", "hat closed.wav", "hat open.wav", "perc 01.wav",
				     "perc 02.wav", "rim.wav", "shaker loop 120.wav", "snare tight.wav", "tom hi.wav", "tom lo.wav",
				     "vinyl hit.wav"};
	static const char *kLibM[] = {"Keys", "Strings", "Upright.sfz", "Rhodes Mk1.sfz", "Wurli 200.sfz", "Celesta.sfz"};
	static const char *kRoot[] = {"@INPUTS", "@BUILT-IN", "GRANULAR", "MULTISAMPLE", "Samples", "WAVETABLES"};
	const char *const *names = l == WGV_INSTR ? kLibM : kLib;
	int n = l == WGV_INSTR ? 6 : 14, dirs = 2;
	snprintf (v->lib_cwd, sizeof v->lib_cwd, "%s", l == WGV_INSTR ? "/MULTISAMPLE" : "/Samples/Drums");
	if (s_WgRoot)
	{
		int nv = l == WGV_SAMPLES ? 2 : l == WGV_TABLES ? 1 : 0;
		names = kRoot + 2 - nv, n = 4 + nv, dirs = n, v->lib_cwd[0] = 0;
	}
	v->lib_n = v->lib_total = n, v->lib_done = 1, v->lib_gen = 77 + (unsigned) l * 3 + (unsigned) s_WgRoot;
	for (int i = 0; i < n; i++)
	{
		snprintf (v->lib_name[i], WGV_LNAME, "%s", names[i]);
		v->lib_flags[i] = (unsigned char) ((i < dirs ? WGV_LIB_DIR : 0) | (names[i][0] == '@' ? WGV_LIB_VIRT : 0));
		v->lib_row[i] = -1;
	}
	if (!s_WgRoot) v->lib_row[2] = (short) (l == WGV_INSTR ? 1 : 2 * 128 + 1);	// 808 kick in C001, Upright in A001
	if (kind == WGV_MULTISAMPLE)			// touch2: an instrument's zones
	{
		snprintf (v->name, sizeof v->name, "UPRIGHT PIANO");
		if (l == WGV_INSTR) v->sel = 1;
		static const unsigned char zl[6] = {21, 36, 48, 60, 72, 84}, zh[6] = {35, 47, 59, 71, 83, 108}, zv[6] = {2, 3, 4, 4, 3, 1};
		v->ms_ready = 1, v->ms_nz = 6;
		for (int z = 0; z < 6; z++) v->ms_lo[z] = zl[z], v->ms_hi[z] = zh[z], v->ms_root[z] = zl[z] + 6, v->ms_vel[z] = zv[z];
		int held[3] = {60, 64, 67 + ((int) (t * 3) & 1) * 2};
		for (int k = 0; k < 3; k++) v->ms_keys[held[k] >> 3] |= (unsigned char) (1u << (held[k] & 7));
	}
	if (kind == WGV_GRANULAR)
	{
		if (l == WGV_SAMPLES) v->sel = 1;			// A001
		snprintf (v->name, sizeof v->name, "kick 909");
		snprintf (v->anim_text, sizeof v->anim_text, "1BAR");
		snprintf (v->hold_text, sizeof v->hold_text, "40");
		v->anim = 0.45f;
		for (int i = 0; i < WGV_COLS; i++)			// eight strikes ringing out
		{
			double ph = fmod (i * 8.0 / WGV_COLS, 1.0), e = exp (-ph * 4.5) * (0.75 + 0.25 * sin (i * 0.9));
			v->wmx[i] = (float) e, v->wmn[i] = (float) -e * 0.92f;
		}
		v->head = (float) (0.35 + 0.18 * sin (t * 6.2832 / 2.0) + 0.04 * sin (t * 6.2832 * 1.5));
		uint32_t r = 12345;
		for (int k = 0; k < 18; k++)
		{
			r = r * 1664525u + 1013904223u;
			double age = fmod (t * 2.5 + k / 18.0, 1.0);			// each grain's life, 0..1
			double sp = ((r >> 8) / 16777216.0 - 0.5) * 0.16;
			v->gpos[v->n] = (float) (v->head - 0.05 + sp + age * 0.03);
			v->gamp[v->n] = (float) (sin (age * 3.1416) * sin (age * 3.1416) * (0.6 + 0.4 * ((r >> 4) & 1)));
			v->grev[v->n] = (r >> 12) % 4 == 0;
			if (v->gpos[v->n] >= 0 && v->gpos[v->n] <= 1) v->n++;
		}
	}
	else if (kind == WGV_WAVETABLE)
	{
		snprintf (v->name, sizeof v->name, "BASIC");
		if (l == WGV_TABLES) v->sel = 1;			// A001
		v->frames = 64;
		v->pos = (float) (0.5 + 0.45 * sin (t * 6.2832 / 4.0));
		for (int k = 0; k < WGV_STACK + 1; k++)
		{
			float p = k < WGV_STACK ? (float) k / (WGV_STACK - 1) : v->pos;
			float *y = k < WGV_STACK ? v->stack[k] : v->cur;
			for (int i = 0; i < WGV_COLS; i++)
			{
				double x = (double) i / WGV_COLS, sn = sin (x * 6.2832), saw = 2 * x - 1 < 0 ? 2 * x : 2 * x - 2,
				       sq = x < 0.5 ? 0.8 : -0.8;
				double u = p * 2;
				y[i] = (float) (u < 1 ? sn * (1 - u) + saw * u : saw * (2 - u) + sq * (u - 1));
			}
		}
	}
}

// the JUNO / 6-OP FM preset pages' view (the Pi reads the preset index and the project's lists): a JUNO list of 40 slots with
// four empty ones, the FM list holding the built-ins (96 rows), and the index: JUNO / FACTORY (128), Pads, DX7 / ROM (96), mine
static sidecar_preset_view_t s_PrV;
static unsigned s_nPreGen, s_nPreSeq;
static char s_PreMsg[40];
static int s_PreBuiltFm = 1;
static const char *const kPreJn[10] = {"Brass", "Strings", "Bass 1", "Pad warm", "Lead saw", "Organ", "Pluck", "Choir", "Sync lead", "Sub bass"};
static const char *const kPreFm[8] = {"BRASS  1", "E.PIANO 1", "STRINGS1", "CLAV  1", "HARP  1", "MARIMBA", "BASS  1", "FLUTE  1"};
static int pre_total (int kind, int f)
{
	if (f < 0) return kind == 0 ? 40 : s_PreBuiltFm ? 96 : 12;
	return f == 0 ? 5 : f == 2 ? 128 : f == 3 ? 9 : f == 1 ? 3 : f == 4 ? 96 : f == 5 ? 32 : 32;
}
static void pre_row (int kind, int f, int i, char *nm, int n, int *flags, int *kid)
{
	*flags = 0, *kid = -1;
	if (f < 0)
	{
		int used = kind == 0 ? i % 11 != 2 : 1;
		*flags = used ? 2 : 0;
		if (!used) nm[0] = 0;
		else if (kind == 0) snprintf (nm, n, "%s", kPreJn[i % 10]);
		else snprintf (nm, n, "%s", kPreFm[i % 8]);
		return;
	}
	if (f == 0) { static const char *const r[5] = {"FACTORY", "Pads", "bass.syx", "lead.syx", "pluck.syx"}; snprintf (nm, n, "%s", r[i]); if (i < 2) *flags = 1, *kid = i ? 3 : 2; }
	else if (f == 1) { static const char *const r[3] = {"ROM", "mine", "rhodes"}; snprintf (nm, n, "%s", r[i]); *flags = 1, *kid = i ? 4 + i : 4; }
	else if (f == 2) snprintf (nm, n, "%c%d%d %s", i < 64 ? 'A' : 'B', (i & 63) / 8 + 1, i % 8 + 1, kPreJn[i % 10]);
	else if (f == 3) snprintf (nm, n, "%s", kPreJn[(i + 3) % 10]);
	else if (f == 4) snprintf (nm, n, "%s", kPreFm[i % 8]);
	else snprintf (nm, n, "VOICE %02d", i + 1);
}
static void pre_fake (void)
{
	sidecar_preset_view_t *v = &s_PrV;
	memset (v, 0, sizeof *v);
	v->kind = sidecar_preset_kind (), v->folder = sidecar_preset_folder (), v->first = sidecar_preset_first ();
	v->ready = 1, v->igen = 7, v->up = -1;
	v->count[0] = 40, v->count[1] = s_PreBuiltFm ? 96 : 12;
	int f = v->folder, t = pre_total (v->kind, f);
	v->total = t;
	v->built_in = f < 0 && v->kind == 1 && s_PreBuiltFm;
	static const char *const kT[7] = {"JUNO", "DX7", "JUNO/FACTORY", "JUNO/Pads", "DX7/ROM", "DX7/mine", "DX7/rhodes"};
	if (f >= 0) snprintf (v->title, sizeof v->title, "%s", kT[f]), v->up = f < 2 ? -1 : f < 4 ? 0 : 1;
	for (int i = 0; i < PRE_ROWS && v->first + i < t; i++)
	{
		int fl, kid;
		pre_row (v->kind, f, v->first + i, v->name[i], PRE_NAME, &fl, &kid);
		v->flags[i] = (unsigned char) fl, v->kid[i] = (short) kid, v->n = i + 1;
	}
	v->msg_seq = s_nPreSeq;
	snprintf (v->msg, sizeof v->msg, "%s", s_PreMsg);
	v->gen = ++s_nPreGen;
}
#define PRE_PUSH()	do { pre_fake (); sidecar_preset_set (&s_PrV); run (2); } while (0)

// ---- the SONG page's made-up take (the Pi fills it from core/song: song_pi.cpp): 32 bars, A01 A02 A03 A01, mutes,
// scenes, a PERF swell, a tempo push, knob lanes (the Rytm's, one of the Sidecar's send FX), two markers
static sidecar_song_view_t s_Sv;
static void song_fake (int state, unsigned playhead)
{
	sidecar_song_view_t *v = &s_Sv;
	memset (v, 0, sizeof *v);
	v->valid = 1, v->state = state;
	strcpy (v->name, "take-004"); strcpy (v->project, "SHARED SET");
	static const char *const rows[SSG_CFG_ROWS] = {"PORT: ENCODER DEST = INT+EXT", "PORT: OUTPUT CH = TRK CH", "PORT: PARAM OUTPUT = NRPN",
						       "SYNC: PROG CH RECEIVE = ON", "SYNC: PROG CH SEND = ON", "SYNC: TRANSPORT SEND = ON"};	// (core/song config_row)
	for (int k = 0; k < SSG_CFG_ROWS; k++) strcpy (v->cfg_row[k], rows[k]);
	v->cfg_wrong = 0;
	strcpy (v->msg, state == SSG_REC ? "recording take-004" : state == SSG_REPLAYING ? "replaying take-004" : "loaded take-004");
	const unsigned B = SSG_BAR;
	v->length = 32 * B, v->playhead = playhead, v->events = 1834, v->bpm10 = 1270, v->pattern = 1, v->takes = 4, v->take_i = 3;
	static const int pats[4] = {0, 1, 2, 0};
	for (int i = 0; i < 4; i++) v->blocks[i] = (ssg_block_t) {i * 8 * B, (i + 1) * 8 * B, (unsigned char) pats[i]};
	v->nblocks = 4;
	int n = 0;
	v->mutes[n++] = (ssg_block_t) {4 * B, 8 * B, 0}; v->mutes[n++] = (ssg_block_t) {12 * B, 20 * B, 2};
	v->mutes[n++] = (ssg_block_t) {16 * B, 24 * B, 8}; v->mutes[n++] = (ssg_block_t) {24 * B, 28 * B, 1};
	v->mutes[n++] = (ssg_block_t) {0, 2 * B, 10}; v->mutes[n++] = (ssg_block_t) {28 * B, 32 * B, 5};
	v->nmutes = n;
	v->scenes[0] = (ssg_block_t) {6 * B, 8 * B, 3}; v->scenes[1] = (ssg_block_t) {22 * B + 8 * SSG_PPQ, 24 * B, 5};
	v->nscenes = 2;
	for (int i = 0; i < 40; i++) v->perf[i] = (ssg_point_t) {14 * B + i * B / 8, (unsigned short) (i < 24 ? i * 650 : (40 - i) * 900)};
	v->nperf = 40;
	v->tempo[0] = (ssg_point_t) {0, 1270}; v->tempo[1] = (ssg_point_t) {16 * B, 1290}; v->tempo[2] = (ssg_point_t) {24 * B, 1310};
	v->ntempo = 3;
	v->marks[0] = 8 * B, v->marks[1] = 24 * B, v->nmarks = 2;
	static const char *const lab[6] = {"BD TUN", "SD FLT A", "CH AMP D", "FX 2.0", "LT SRC C", "SC SND4"};	// (replay.cpp param_label)
	for (int k = 0; k < 6; k++)
	{
		ssg_lane_t *L = &v->lane[k];
		strcpy (L->label, lab[k]);
		L->sidecar = k == 5;
		for (int i = 0; i < 120; i++)
		{
			double x = i / 120.0;
			double y = k == 0 ? 0.5 + 0.4 * sin (x * 12) : k == 1 ? x : k == 2 ? (i / 15 % 2 ? 0.8 : 0.2) : k == 3 ? 0.3 + 0.6 * x * x :
				   k == 4 ? 0.5 + 0.45 * sin (x * 40) * x : 0.2 + 0.6 * (x > 0.5);
			L->p[i] = (ssg_point_t) {(unsigned) (2 * B + x * 28 * B), (unsigned short) (y * 16383)};
		}
		L->n = 120;
	}
	v->nlanes = 6;
	v->gen = 0x1000u + (unsigned) state * 7u + playhead;
}

// ---- the REC page's made-up recorder (the Pi fills it from pi/engine/recorder.h: rec_pi.cpp UiFill): MASTER, four
// stems, two voices and FX 1 armed; levels moving, MAIN L clipping; five takes on the card
static sidecar_rec_view_t s_Rv;
static int s_PlSt;				// a take's playback in the fake: SRV_PL_*, which take, elapsed ms
static const char *s_PlTake = "";
static unsigned s_PlMs, s_PlLenMs;
static int s_RecUsb;				// the destination a USB drive (rec=vol:USB): take_003 is on it
static int s_RecMany;				// 5d: 23 takes named by project and date (newest first): the list scrolls
static int s_RecWarn;				// 5d: the selection needs 82 % of the card's measured speed: REC / ARM ask first
static char s_RecMany_[23][SRV_NAME];		// (their names)
static const char *rec_many (int i)
{
	if (i == 3) snprintf (s_RecMany_[i], SRV_NAME, "USB:jam-night_2026-10-%02d_2130", 27 - i);
	else if (i == 1) snprintf (s_RecMany_[i], SRV_NAME, "jam-night_2026-10-27_2130_2");
	else if (i >= 20) snprintf (s_RecMany_[i], SRV_NAME, "jam-night_take-%03d", 23 - i);	// before the clock was set
	else snprintf (s_RecMany_[i], SRV_NAME, "jam-night_2026-10-%02d_2130", 27 - i);
	return s_RecMany_[i];
}
static void rec_fake (int state, unsigned secs, int frame)
{
	static const char *const nm[SRV_CH] = {"MASTER", "MAIN L", "MAIN R", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB",
					       "IN L", "IN R", "BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP", "FX 1", "FX 2"};
	sidecar_rec_view_t *v = &s_Rv;
	memset (v, 0, sizeof *v);
	v->valid = 1, v->state = state, v->bits = 24, v->rate = 48000, v->secs = secs;
	strcpy (v->take, state == SRV_REC || state == SRV_SAVING ? "take_005" : "take_004");
	v->free_mb = 12650, v->total_mb = 30420;
	v->mask = 1u << 0 | 1u << 3 | 1u << 4 | 1u << 6 | 1u << 9 | 1u << 13 | 1u << 14 | 1u << 21;
	v->need_kbps = (2 + 4 + 2 + 2) * 3 * 48000 / 1024;
	for (int i = 0; i < SRV_CH; i++)
	{
		strcpy (v->name[i], nm[i]);
		for (int k = 0; k < 2; k++)
		{
			double ph = frame * 0.37 + i * 1.7 + k * 0.6, lvl = 0.5 + 0.5 * sin (ph);
			double base = i == 5 || i == 7 || i == 10 || i == 12 || i == 20 || i == 22 ? 0.0 : 0.12 + 0.55 * lvl * (0.4 + (i % 5) * 0.15);
			v->peak[i][k] = (unsigned) (base * SRV_FS);
		}
	}
	v->peak[1][0] = SRV_FS; v->peak[0][0] = SRV_FS;		// MAIN L (MASTER's L): a clip
	v->peak[11][0] = SRV_FS / 300;				// IN L: a whisper
	v->ntakes = 5;
	static const char *const tk[5] = {"take_004", "take_003", "take_002", "jam_night_001", "take_001"};
	static const unsigned ts[5] = {83, 412, 3725, 1260, 9};
	for (int i = 0; i < 5; i++)
	{
		strcpy (v->takes[i].name, tk[i]);
		v->takes[i].secs = ts[i];
		v->takes[i].files = 8 - i;
		v->takes[i].kb = (unsigned) ((unsigned long long) ts[i] * (8 - i) * 144);
	}
	v->takes_gen = 7;
	v->vol_missing = s_RecUsb == 2, v->free_unknown = s_RecUsb == 3;
	if (s_RecUsb) strcpy (v->vol, "USB"), strcpy (v->takes[1].name, "USB:take_003"), v->free_mb = s_RecUsb == 1 ? 58982 : 0, v->total_mb = s_RecUsb == 2 ? 0 : 61056, v->takes_gen = 8;
	if (s_RecMany)
	{
		v->ntakes = 23;
		for (int i = 0; i < 23; i++)
		{
			strcpy (v->takes[i].name, rec_many (i));
			v->takes[i].secs = 60 + i * 37, v->takes[i].files = 1 + i % 9, v->takes[i].kb = (unsigned) (v->takes[i].secs * (1 + i % 9) * 144);
		}
		v->takes_gen = 9;
	}
	v->speed_kbps = 5120, v->speed_measured = 1;		// the card's bench: 5.0 MB/s
	if (s_RecWarn) v->need_kbps = 4218, v->mask = (1u << SRV_CH) - 1, v->warn_pct = 82;	// every channel, 24-bit
	v->pl_state = s_PlSt;
	if (s_PlSt) strcpy (v->pl_take, s_PlTake), v->pl_pos_ms = s_PlMs, v->pl_len_ms = s_PlLenMs;
}
static void rec_play (int state, unsigned secs, int frames)	// a few views 40 ms apart: the meters move
{
	for (int f = 0; f < frames; f++) { rec_fake (state, secs, f); sidecar_rec_set (&s_Rv); run (1); }
}

// ---- the pages of one theme, their controls' commands, the SEQ gestures
// The taps are the 800x480 layouts' places in LU, moved by what the screen adds (DX, DY: TH->dx, dy) the way the pages
// move their controls (sidecar_ui.c, sidecar_seqed.c, ...): the same checks run at all three sizes.
static int DX;
#define OW_HIT_X	(LU (352) + DX - LU (40))						// OLED: the home title bar's REC status
#define NAV_X		(LU (377) + DX)						// OLED: the navigation column
#define NAV_Y(i)	((i) * ((LU (240) + TH->dy) / 8) + (LU (240) + TH->dy) / 16 - 1)		// eight keys (UX review N1)
#define CNAV_X(i)	(TH->nav ? TH->w - TH->nav / 2 : LU (300) + (i) * ((LU (800) + DX - LU (12) - LU (300) + LU (4)) / 8) + ((LU (800) + DX - LU (12) - LU (300) + LU (4)) / 8 - LU (4)) / 2)	// SIDECAR: the rail, or the header's row (800x480)
#define PDY		(TH->pdy)							// HOME: laid out below the sub-page row
#define SUBW(n)		(o ? ((n) * LU (60) < LU (352) + DX ? (n) * LU (60) : LU (352) + DX) : ((n) * LU (130) < LU (776) + DX ? (n) * LU (130) : LU (776) + DX))	// the sub-page row of n pills
#define SUBX(n, k)	((o ? 0 : LU (12)) + SUBW (n) * (2 * (k) + 1) / (2 * (n)))
#define SUBY		(o ? LU (27) : LU (80))
#define CNAV_Y(i)	(TH->nav ? LU (3) + (i) * ((TH->h - LU (6)) / 8) + (TH->h - LU (6)) / 16 : LU (25))
#define VS(v)		((v) * (LU (240) + DY - LU (41)) / 199)				// OLED: a settings page's y (sidecar_ui.c)
#define CY(v)		((v) * (LU (358) + DY) / 358)				// SIDECAR: the same
#define CH		(LU (48) + DY / 15)
#define WG_KY		(o ? LU (204) + DY + LU (17) : LU (52) + LU (372) + DY + LU (26))		// the machine view's keys (sidecar_wgview.c)
#define WG_SRC_X	(o ? LU (264) + 3 * DX / 4 + (LU (88) + DX / 4) / 2 : LU (616) + 3 * DX / 4 + (LU (176) + DX / 4) / 2)
#define WG_MID_X	(o ? LU (180) + DX / 2 + (LU (78) + DX / 4) / 2 : LU (312) + DX / 2 + (LU (296) + DX / 4) / 2)	// ALL POOLS / UNUSED / FILL
#define WG_DN_X		(o ? LU (90) + DX / 4 + (LU (84) + DX / 4) / 2 : LU (164) + DX / 4 + (LU (140) + DX / 4) / 2)
// ---- the PROJECT page's view (core/bundle/feed.h's Feed as the kernel copies it)
static sidecar_project_view_t s_Pv;
static void project_fake (int phase)
{
	sidecar_project_view_t *v = &s_Pv;
	unsigned gen = v->gen, seq = v->seq;
	memset (v, 0, sizeof *v);
	v->valid = 1, v->gen = gen, v->seq = seq + 1;
	static const char *const nm[6] = {"AR PERC PATCH", "LIVE SET 3", "JAM 1021", "AR PERC PATCH 2", "A/B TEST", "SUNDAY"};
	static const char *const ky[6] = {"AR PERC PATCH", "LIVE SET 3", "JAM 1021", "AR PERC PATCH 2", "A_B TEST", "SUNDAY"};
	static const unsigned long long by[6] = {412ull << 20, 88ull << 20, 12ull << 20, 412ull << 20, 3ull << 20, 40000};
	v->nproj = 6;
	for (int i = 0; i < 6; i++)
	{
		spj_project_t *p = &v->proj[i];
		snprintf (p->key, sizeof p->key, "%s", ky[i]);
		snprintf (p->rytm, sizeof p->rytm, "%s", nm[i]);
		p->files = 40 - 5 * i, p->bytes = by[i], p->used = 9 - i, p->current = i == 0, p->imported = i == 3;
		if (i == 1) snprintf (p->date, sizeof p->date, "2026-10-22T14:03:00Z");
	}
	v->nvol = 1;
	snprintf (v->vol[0].drive, 8, "USB:"), snprintf (v->vol[0].name, 8, "USB");
	v->vol[0].free = 16000ull << 20, v->vol[0].total = 30000ull << 20, v->vol[0].free_known = 1;
	v->card_free = 42200ull << 20;
	v->nbund = 3;
	static const char *const bn[3] = {"AR PERC PATCH 20261022-1403.sidecar.zip", "LIVE SET 3 001.sidecar.zip", "JAM 1021 20261020-2210.sidecar.zip.part"};
	for (int i = 0; i < 3; i++)
	{
		snprintf (v->bund[i].name, sizeof v->bund[i].name, "%s", bn[i]);
		snprintf (v->bund[i].path, sizeof v->bund[i].path, "/Bundles/%s", bn[i]);
		snprintf (v->bund[i].date, sizeof v->bund[i].date, "2026-10-2%d 14:0%d", 2 - i, i);
		v->bund[i].size = (412ull << 20) >> i, v->bund[i].incomplete = i == 2;
	}
	// 2026-10-27: the project trash (core/bundle/prjslot.h), the newest first
	static const char *const tn[3] = {"PRESETS", "JAM 1021", ""}, *const tk[3] = {"@005", "@012", "@002"};
	static const char *const tw[3] = {"clear", "saveto", "web"};
	v->ntrash = 3;
	for (int i = 0; i < 3; i++)
	{
		spj_trash_t *t = &v->trash[i];
		t->id = 12 - 3 * i, t->items = 5 - i;
		snprintf (t->key, sizeof t->key, "%s", tk[i]), snprintf (t->name, sizeof t->name, "%s", tn[i]);
		snprintf (t->why, sizeof t->why, "%s", tw[i]);
		if (i == 0) snprintf (t->date, sizeof t->date, "2026-10-27T10:03:00Z");
	}
	v->phase = phase;
	if (phase == SPJ_WRITE)
	{
		v->job = SPJ_JOB_EXPORT, v->done = 163ull << 20, v->total = 412ull << 20;
		snprintf (v->key, sizeof v->key, "AR PERC PATCH");
		snprintf (v->waiting, sizeof v->waiting, "PLAYING");
	}
	if (phase == SPJ_READY || phase == SPJ_DONE)
	{
		v->job = SPJ_JOB_IMPORT, v->have_plan = 1;
		snprintf (v->key, sizeof v->key, "AR PERC PATCH 2");
		snprintf (v->plan_name, 24, "AR PERC PATCH 2"), snprintf (v->plan_from, 24, "AR PERC PATCH");
		snprintf (v->plan_step, sizeof v->plan_step, "Send AR PERC PATCH.arprj to the Rytm with Elektron Transfer, then save it on the Rytm as AR PERC PATCH 2.");
		snprintf (v->plan_rytm, sizeof v->plan_rytm, "AR PERC PATCH.arprj");
		v->plan_clash = 1, v->plan_new = 120ull << 20, v->plan_reused = 292ull << 20;
		v->plan_write = 31, v->plan_reuse = 149, v->plan_samples = 180, v->plan_songs = 4, v->plan_patterns = 37;
		v->nmach = 2;
		v->mach[0].slot = 1, v->mach[0].action = SPJ_PKG_INSTALL, snprintf (v->mach[0].name, 12, "GRAIN"), snprintf (v->mach[0].ver, 12, "1.0.0");
		v->mach[1].slot = 2, v->mach[1].action = SPJ_PKG_REUSE, snprintf (v->mach[1].name, 12, "ACID"), snprintf (v->mach[1].ver, 12, "0.3.1");
		if (phase == SPJ_DONE) snprintf (v->msg, sizeof v->msg, "imported as AR PERC PATCH 2: 31 files written, 149 already on the card");
	}
	if (phase == SPJ_FAILED)
	{
		v->job = SPJ_JOB_EXPORT;
		snprintf (v->key, sizeof v->key, "LIVE SET 3");
		snprintf (v->msg, sizeof v->msg, "card full: 1240 MB needed, 410 MB free - delete recordings or use a USB stick");
	}
}
static void project_tap (int what, int i)
{
	int x, y;
	if (!sidecar_project_hit (what, i, &x, &y)) { printf ("FAIL project: control %d/%d not shown [%s]\n", what, i, s_Pre); s_nFail++; return; }
	tap_raw (x, y);
}

// CHOP's view as chopview_pi.cpp fills it: a two-bar break (kicks, snares, hats) in 12 slices on voice 0 (BD), the
// outline for the page's window, two playheads; LAZY with three markers; a RESAMPLE's offer
static sidecar_chop_view_t s_Ch;
static void chop_fake (int mode)
{
	sidecar_chop_view_t *v = &s_Ch;
	static const char *const kF[SCH_FIELDS] = {"PTCH", "FINE", "GAIN", "REV", "ATK", "DEC", "LOOP", "CHOK", "NUDG", "STUT", "SLEN",
						   "SDEC", "TAPE", "TTIM", "NBR", "POLY", "GATE"};
	static const int kLo[SCH_FIELDS] = {-24, -50, -24, 0, 0, 0, 0, 0, -2400, 1, 1, 0, 0, 20, 0, 0, 0};
	static const int kHi[SCH_FIELDS] = {24, 50, 12, 1, 2000, 10000, 2, 4, 2400, 16, 100, 90, 2, 4000, 100, 1, 1};
	unsigned gen = v->gen;
	memset (v, 0, sizeof *v);
	v->gen = gen + 1;
	if (mode < 0) return;					// no CHOP voice
	v->valid = 1, v->voice = 0, v->track = 0, v->ready = 1, v->frames = 192000, v->rate = 48000;
	snprintf (v->name, sizeof v->name, "AMEN BREAK 136");
	static const unsigned kSt[12] = {0, 14100, 28200, 42300, 56400, 70600, 84700, 98800, 112900, 127000, 141200, 169400};
	v->n = 12;
	for (int k = 0; k < 12; k++) v->start[k] = kSt[k], v->end[k] = k < 11 ? kSt[k + 1] : 192000;
	for (int f = 0; f < SCH_FIELDS; f++) { snprintf (v->fname[f], 6, "%s", kF[f]); v->flo[f] = kLo[f], v->fhi[f] = kHi[f]; }
	for (int k = 0; k < 12; k++)
		for (int f = 0; f < SCH_FIELDS; f++)
		{
			int x = f == 9 ? 1 : f == 10 ? 25 : f == 11 ? 20 : f == 13 ? 300 : 0;
			if (k == 3 && f == 0) x = 5;
			if (k == 3 && f == 3) x = 1;
			if (k == 5 && f == 9) x = 4;
			if (k == 7 && f == 12) x = 1;
			if (k == 2 && f == 7) x = 1;
			v->field[k][f] = x;
			static const char *const kLoop[3] = {"OFF", "FWD", "PING"}, *const kTape[3] = {"OFF", "STOP", "SPIN"};
			char *t = v->ftext[k][f];
			if (f == 0) snprintf (t, SCH_TEXT, x > 0 ? "+%d" : "%d", x);
			else if (f == 1) snprintf (t, SCH_TEXT, "%dC", x);
			else if (f == 2) snprintf (t, SCH_TEXT, "%dDB", x);
			else if (f == 3) snprintf (t, SCH_TEXT, x ? "REV" : "FWD");
			else if (f == 4 || f == 13) snprintf (t, SCH_TEXT, "%dMS", x);
			else if (f == 5) snprintf (t, SCH_TEXT, "HOLD");
			else if (f == 6) snprintf (t, SCH_TEXT, "%s", kLoop[x]);
			else if (f == 7) { if (x) snprintf (t, SCH_TEXT, "G%d", x); else snprintf (t, SCH_TEXT, "OFF"); }
			else if (f == 8) snprintf (t, SCH_TEXT, "0.0MS");
			else if (f == 9) { if (x > 1) snprintf (t, SCH_TEXT, "X%d", x); else snprintf (t, SCH_TEXT, "OFF"); }
			else if (f == 12) snprintf (t, SCH_TEXT, "%s", kTape[x]);
			else if (f >= 15) snprintf (t, SCH_TEXT, "%s", x ? "ON" : "OFF");
			else snprintf (t, SCH_TEXT, "%d%%", x);
		}
	sidecar_chop_range (v->frames, &v->a, &v->b);
	v->ncols = SCH_COLS;
	for (int i = 0; i < SCH_COLS; i++)
	{
		unsigned f = v->a + (unsigned) ((unsigned long long) (v->b - v->a) * i / SCH_COLS);
		unsigned ph = f % 24000;				// a hit every 24000 frames, kick / snare / hat by turn
		int kind = (int) (f / 24000) % 3;
		double env = kind == 0 ? exp (-(double) ph / 9000.0) : kind == 1 ? exp (-(double) ph / 6000.0) : exp (-(double) ph / 2500.0);
		double a = (kind == 0 ? 0.9 : kind == 1 ? 0.7 : 0.35) * env * (0.75 + 0.25 * sin (i * 1.7));
		v->hi[i] = (short) (a * 32000), v->lo[i] = (short) (-a * 30000);
	}
	v->nheads = 2, v->head[0] = 44000, v->head[1] = 101000;
	v->slc = 3, v->played = 3, v->dirty = 1, v->lat = 336;
	if (mode == 1)
	{
		v->lazy = 1, v->lazy_n = 3, v->lazy_at[0] = 0, v->lazy_at[1] = 24100, v->lazy_at[2] = 48050, v->lazy_pos = 61000, v->nheads = 0;
	}
	if (mode == 2) v->rs_state = SCH_RS_OFFER, v->rs_slice = 3, snprintf (v->rs_row, sizeof v->rs_row, "A012");
}

// SAMPLE EDIT's view as seditview_pi.cpp fills it: 0 a kick opened from track BD (the region 0.05..1.0 s, an edit to
// undo, not saved), 1 a stereo pad from the pool (no track: no SAVE + USE; nothing changed), 2 a NORMALISE at 42 %
static sidecar_sedit_view_t s_Se;
static void sedit_fake (int mode)
{
	sidecar_sedit_view_t *v = &s_Se;
	unsigned gen = v->gen;
	memset (v, 0, sizeof *v);
	v->gen = gen + 1;
	v->valid = 1, v->st = mode == 2 || mode == 3 ? SSE_ST_BUSY : SSE_ST_READY, v->rate = 48000, v->bits = 24, v->fn = mode == 2 ? 1 : 0;
	v->frames = mode == 1 ? 192000 : mode == 4 ? 131072 : 57600, v->ch = mode == 1 ? 2 : 1;
	if (mode == 4) v->bits = 32;
	snprintf (v->name, sizeof v->name, "%s", mode == 1 ? "PAD WARM C3.wav" : mode == 4 ? "saw-wt1.wav" : "KICK 808 LONG.wav");
	v->a = mode == 1 ? 0 : 2400, v->b = mode == 1 || mode == 4 ? v->frames : 48000;
	v->wt_frames = 64, v->wt_voice = mode == 4 ? 2 : -1, v->file = mode == 4, v->making = mode == 3;
	v->wa = 0, v->wb = v->frames;
	v->undo = mode == 0 || mode == 2, v->dirty = mode == 0 || mode == 2, v->has_track = mode == 0 || mode == 2, v->track = mode == 0 || mode == 2 ? 0 : -1;
	v->progress = mode == 2 || mode == 3 ? 42 : 100, v->playhead = -1, v->ncols = SSE_COLS, v->unity = 32767;
	for (int i = 0; i < SSE_COLS; i++)
	{
		double t = (double) i / SSE_COLS, a;
		if (mode == 1) a = 0.55 * sin (3.14159 * t) * (0.8 + 0.2 * sin (i * 0.37));	// a pad's swell
		else a = 0.95 * exp (-t * 4.0) * (0.7 + 0.3 * fabs (sin (i * 0.21)));		// a kick's decay
		v->hi[i] = (short) (a * 32000), v->lo[i] = (short) (-a * 31000);
	}
}

static void contrast_check (void);
// the machine view's list: its i-th row as counted from the list's top (the tab rows first), y on the screen; a row below or
// above the rows shown is brought in with DOWN / UP first (0.5.1: 36 px rows, at 800x480 fewer than the list has show)
static int wg_list_y (int i, int wly, int wlr)
{
	const int o = TH->id == THEME_OLED;
	int top, head, n = sidecar_wgview_rows (&top, &head);
	for (int k = 0; k < 16 && i >= head && (i - head < top || i - head >= top + n); k++)
	{
		tap (i - head < top ? (o ? (LU (84) + DX / 4) / 2 : LU (16) + (LU (140) + DX / 4) / 2) : WG_DN_X, WG_KY);
		n = sidecar_wgview_rows (&top, &head);
	}
	return wly + (i < head ? i : head + i - head - top) * wlr + wlr / 2;
}

static void theme_run (int style, const char *pre)
{
	snprintf (s_Pre, sizeof s_Pre, "%s", pre);
	int o = style == THEME_OLED;
	status_init (style);
	sidecar_ui_status (&s_St);
	DX = TH->dx;
	EXPECT (sidecar_ui_get_style () == style);
	EXPECT (sidecar_ui_dot () == (o ? SIDECAR_DOT : 1));
	if (TH->hc) contrast_check ();
	sidecar_ui_touch (1, 2010, 1240, 392, 145);
	sidecar_ui_show (0); shot ("home-touch");			// (the touch test's ring under the finger: not on the home render)
	EXPECT (sidecar_ui_touch_test_shown ());
	sidecar_ui_touch (0, 2010, 1240, 392, 145);			// T1: the first tap seen hides HOME's touch test (stored)
	EXPECT (!sidecar_ui_touch_test_shown ()); EXPECT_CMD ("touchtest=0");
	shot ("home");
	sidecar_ui_show (1); shot ("sound");
	// the USB-C mode: OVERBRIDGE set with the box shown (its warning), not in this build (greyed), no Sidecar gadget
	s_St.usb_mode = 1, s_St.usb_shown = 1; sidecar_ui_status (&s_St); run (2); shot ("sound-overbridge");
	s_St.usb_modes = 1, s_St.usb_shown = 0; sidecar_ui_status (&s_St); run (2); shot ("sound-overbridge-nobuild");
	s_St.usb_modes = 0, s_St.usb_shown = -1; sidecar_ui_status (&s_St); run (2); shot ("sound-usb-nogadget");
	s_St.usb_mode = 0, s_St.usb_modes = 3, s_St.usb_shown = 0; sidecar_ui_status (&s_St); run (2);
	sidecar_ui_show (23); shot ("midi");
	// Settings > MIDI: CHANNEL, the computer's notes off, nothing plugged in, a locked learn file; a build without keys
	s_St.keys_mode = 1, s_St.keys_mac = 0, s_St.keys_in[0] = 0, s_St.keys_last[0] = 0, s_St.learn_routes = -1;
	sidecar_ui_status (&s_St); run (2); shot ("midi-channel");
	s_St.keys_auto = 0; sidecar_ui_status (&s_St); run (2); shot ("midi-nokeys");
	s_St.keys_mode = 0, s_St.keys_auto = 14, s_St.keys_mac = 1, s_St.learn_routes = 2;
	strcpy (s_St.keys_in, "MPKMINI, LAUNCHCONTROLXL"); strcpy (s_St.keys_last, "MPKMINI > SD POLY");
	sidecar_ui_status (&s_St); run (2);
	sidecar_ui_show (2); shot ("fx");
	sidecar_ui_show (6); shot ("display");
	if (!o)		// the UI style row names its three themes (a tap by position would not notice a stale label)
	{
		EXPECT (text_shown (lv_screen_active (), "OLED") && text_shown (lv_screen_active (), "Sidecar") && text_shown (lv_screen_active (), "Grid"));
		EXPECT (!text_shown (lv_screen_active (), "Guide") && !text_shown (lv_screen_active (), "Square"));
	}
	{
		// the touch calibration: 4 crosses, the raw values at each release; a linear 0..32767 panel gives the full
		// range; CANCEL in the middle ends it without a command; RESET sends touchcal=off
		int n0 = s_nCmds;
		sidecar_ui_touch_cal_start ();
		EXPECT (sidecar_ui_touch_cal_step () == 0);
		run (2); shot ("touchcal");
		for (int i = 0; i < 4; i++)
		{
			int fx = i == 1 || i == 2 ? 900 : 100, fy = i >= 2 ? 900 : 100;
			int ux = TH->w * fx / 1000, uy = TH->h * fy / 1000, rx = fx * 32767 / 1000, ry = fy * 32767 / 1000;
			sidecar_ui_touch (1, rx, ry, sx (ux), sy (uy));
			sidecar_ui_touch (0, rx, ry, sx (ux), sy (uy));
			EXPECT (sidecar_ui_touch_cal_step () == (i < 3 ? i + 1 : -1));
		}
		EXPECT_CMD ("touchcal=0,32766,0,32766,0");
		EXPECT (s_nCmds == n0 + 1);
		sidecar_ui_touch_cal_start ();
		int cx = TH->w / 2, cy = TH->h / 2 + (o ? LU (30) : LU (70));
		sidecar_ui_touch (1, 16000, 16000, sx (cx), sy (cy));
		sidecar_ui_touch (0, 16000, 16000, sx (cx), sy (cy));
		EXPECT (sidecar_ui_touch_cal_step () == -1 && s_nCmds == n0 + 1);	// cancelled
		// a bad set (all four at one place): asked again, no command
		sidecar_ui_touch_cal_start ();
		for (int i = 0; i < 4; i++)
		{
			int fx = i == 1 || i == 2 ? 900 : 100, fy = i >= 2 ? 900 : 100;
			sidecar_ui_touch (1, 100, 100, sx (TH->w * fx / 1000), sy (TH->h * fy / 1000));
			sidecar_ui_touch (0, 100, 100, sx (TH->w * fx / 1000), sy (TH->h * fy / 1000));
		}
		EXPECT (sidecar_ui_touch_cal_step () == 0 && s_nCmds == n0 + 1);
		run (2); shot ("touchcal-again");
		sidecar_ui_touch (1, 16000, 16000, sx (cx), sy (cy));
		sidecar_ui_touch (0, 16000, 16000, sx (cx), sy (cy));
		EXPECT (sidecar_ui_touch_cal_step () == -1);
		run (2);
		s_St.touch_cal = 1; sidecar_ui_status (&s_St); shot ("display-calibrated");
		s_St.touch_cal = 0; sidecar_ui_status (&s_St);
	}
	sidecar_ui_show (3); shot ("about");
	sidecar_ui_show (7); wg_fake (WGV_GRANULAR, 0.3); sidecar_wgview_set (&s_Wg); run (2); shot ("mach-granular");
	EXPECT (sidecar_wgview_shown ());
	{
	// the POOL (core/lib/pools.h): the list's rows below a tab row (the POOL page: two), FILES aimed at a row, a row's
	// menu (a long press), the P3 confirm, the message row (UNDO), SELECT UNUSED
	const int wlr = o ? LU (18) : LU (36), wly = o ? LU (16) : LU (52) + LU (44), wlx = o ? LU (150) : LU (24) + LU (200), lh = (o ? LU (180) : LU (310)) + DY;
	const int tx0 = o ? 0 : LU (24), rw = (o ? LU (352) - LU (5) : LU (752) - LU (8)) + DX - 3, tw = rw / 8, tw5 = rw / 5;
	const int yes_x = tx0 + (o ? LU (8) : LU (40)) + LU (12), no_x = tx0 + rw - (o ? LU (8) : LU (40)) - LU (12);
#define WG_ROW(i)	wg_list_y (i, wly, wlr)				// the list area's i-th row (counted from the list's top: scrolled to it)
#define WG_BOX(n, j)	(wly + (lh - (n) * wlr) / 2 + (j) * wlr + wlr / 2)	// the box of n rows' j-th
#define WG_MSG		(wly + (lh / wlr - 1) * wlr + wlr / 2)			// the message row
#define WG_PUSH()	do { wg_fake (s_Wg.kind, 0.3); sidecar_wgview_set (&s_Wg); run (2); } while (0)
	// GRANULAR's POOL: SAMPLES bank A (the voice plays A001); a filled row: the voice plays it (gr=pick, the Rytm's SRC)
	tap (WG_SRC_X, WG_KY); WG_PUSH (); shot ("pool-samples");
	EXPECT (sidecar_wgview_list_shown () && sidecar_wgview_list_id () == WGV_SAMPLES && sidecar_wgview_bank () == 0);
	tap (wlx, WG_ROW (1 + 2)); EXPECT_CMD ("gr=pick:2:A002");
	// an empty row: FILES aimed at it (the browser: up, a folder, the root), a file into it, then the next empty row
	tap (wlx, WG_ROW (1 + 7)); WG_PUSH ();
	EXPECT (sidecar_wgview_lib_shown () && sidecar_wgview_target () == 7);
	shot ("pool-files");
	tap (wlx, WG_ROW (1)); EXPECT_CMD ("browse=gr:up");
	tap (wlx, WG_ROW (2)); EXPECT_CMD ("browse=gr:open:0");
	tap (wlx, WG_ROW (0)); EXPECT_CMD ("browse=gr:root");
	tap (wlx, WG_ROW (5)); EXPECT_CMD ("pool=smp:assign:A007:3");				// clap.aif into A007
	EXPECT (sidecar_wgview_target () == 8);
	tap (WG_MID_X, WG_KY); EXPECT_CMD ("pool=smp:fill:A:/Samples/Drums");			// FILL A
	EXPECT (!sidecar_wgview_lib_shown () && sidecar_wgview_list_shown ());
	// one entry per hash: a file already in the list shows its row (IN C001); a tap goes there, nothing sent
	tap (wlx, WG_MSG);			// (the message row: FILL's note, cleared)
	tap (wlx, WG_ROW (1 + 8)); WG_PUSH ();
	s_nCmds = 0; tap (wlx, WG_ROW (4));
	EXPECT (s_nCmds == 0 && sidecar_wgview_bank () == 2 && !sidecar_wgview_lib_shown ());
	EXPECT (!strcmp (sidecar_wgview_message (), "ALREADY IN THE POOL AT C001"));
	WG_PUSH (); shot ("pool-existing");
	// FILES at the root: the virtual folders the list takes (@INPUTS/ @BUILT-IN/)
	s_WgRoot = 1;
	tap (wlx, WG_ROW (1 + 4)); WG_PUSH ();
	EXPECT (sidecar_wgview_target () == 2 * 128 + 4);
	shot ("pool-files-root");
	s_WgRoot = 0;
	tap (WG_SRC_X, WG_KY);			// BACK
	EXPECT (!sidecar_wgview_lib_shown () && sidecar_wgview_list_shown ());
	// a long press: the row's menu; UNLOAD of a row in use asks first (P3: C001, 3 voices on it), YES with ":yes",
	// the message row then offers UNDO
	hold (wlx, WG_ROW (1 + 1)); WG_PUSH ();
	EXPECT (sidecar_wgview_asking () == 2);
	shot ("pool-menu");
	tap (wlx, WG_BOX (5, 3));		// UNLOAD
	EXPECT (sidecar_wgview_asking () == 1 && !strcmp (sidecar_wgview_message (), "ROW C001 USED BY 3 SOUNDS."));
	WG_PUSH (); shot ("pool-confirm");
	tap (yes_x, WG_BOX (3, 2)); EXPECT_CMD ("pool=smp:unload:C001:yes");
	EXPECT (strstr (sidecar_wgview_message (), "UNDO") != 0);
	WG_PUSH (); shot ("pool-undo");
	tap (wlx, WG_MSG); EXPECT_CMD ("pool=smp:undo");
	// REPLACE (C003: seen playing): FILES aimed at it, the file asked first, then sent with ":yes"
	hold (wlx, WG_ROW (1 + 3)); tap (wlx, WG_BOX (5, 2));
	EXPECT (sidecar_wgview_lib_shown () && sidecar_wgview_target () == 2 * 128 + 3);
	WG_PUSH (); shot ("pool-replace-files");
	tap (wlx, WG_ROW (5));
	EXPECT (sidecar_wgview_asking () == 1 && !strcmp (sidecar_wgview_message (), "ROW C003 SEEN PLAYING IN THIS PROJECT."));
	tap (yes_x, WG_BOX (3, 2)); EXPECT_CMD ("pool=smp:assign:C003:3:yes");
	EXPECT (!sidecar_wgview_lib_shown ());
	// while the Rytm plays: a row in use is refused (no command); a row not in use is unloaded at once
	s_WgPlaying = 1; WG_PUSH ();
	s_nCmds = 0;
	hold (wlx, WG_ROW (1 + 1)); tap (wlx, WG_BOX (5, 3));
	EXPECT (s_nCmds == 0 && !sidecar_wgview_asking () && strstr (sidecar_wgview_message (), "THE RYTM PLAYS") != 0);
	WG_PUSH (); shot ("pool-playing");
	hold (wlx, WG_ROW (1 + 2)); tap (wlx, WG_BOX (5, 3)); EXPECT_CMD ("pool=smp:unload:C002");
	s_WgPlaying = 0;
	// a bank tab: shown (takes go there); bank H: the inputs and built-ins
	tap (tx0 + 7 * tw + tw / 2, WG_ROW (0)); EXPECT_CMD ("pool=smp:shown:H");
	WG_PUSH (); shot ("pool-samples-bank-h");
	tap (tx0 + tw / 2, WG_ROW (0)); EXPECT_CMD ("pool=smp:shown:A");
	// SELECT UNUSED: the rows not seen playing selected (A003 deselected by a tap), UNLOAD 14 asked with the warning
	tap (WG_MID_X, WG_KY); WG_PUSH ();
	EXPECT (sidecar_wgview_selecting ());
	shot ("pool-unused");
	tap (wlx, WG_ROW (1 + 3)); WG_PUSH ();
	tap (WG_MID_X, WG_KY); WG_PUSH ();
	EXPECT (sidecar_wgview_asking () == 1 && !strcmp (sidecar_wgview_message (), "UNLOAD 14 ROWS FROM SAMPLES?"));
	shot ("pool-unused-confirm");
	s_nCmds = 0; tap (yes_x, WG_BOX (3, 2));
	EXPECT (s_nCmds == 1 && !strncmp (s_Last, "pool=smp:unload:A005:yes,A006:yes,C002:yes,H002:yes,", 52));
	EXPECT (!sidecar_wgview_selecting ());
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (!sidecar_wgview_list_shown ());
	run (2); shot ("mach-granular-done");

	// WAVETABLE's POOL: WAVETABLES (the built-ins in A001..A006); a pick; an empty row filled from FILES; a long press on
	// a built-in: UNLOAD only, asked (A001: a voice on it), NO sends nothing
	wg_fake (WGV_WAVETABLE, 0.3); sidecar_wgview_set (&s_Wg); run (2); shot ("mach-wavetable");
	{	// EDIT (where UP stands in the lists): table A's card file in SAMPLE EDIT, else the SAMPLES list to pick a sample from
		const int ed_x = o ? (LU (84) + DX / 4) / 2 : LU (16) + (LU (140) + DX / 4) / 2;
		s_Wg.tbl_file = 1, s_Wg.gen++; sidecar_wgview_set (&s_Wg); run (2);
		tap (ed_x, WG_KY); EXPECT_CMD ("sedit=open:wt:2");
		s_Wg.track = -1, s_Wg.gen++; sidecar_wgview_set (&s_Wg); run (2);
		tap (ed_x, WG_KY); EXPECT_CMD ("sedit=open:wtv:2");
		s_Wg.tbl_file = 0, s_Wg.track = 2, s_Wg.gen++; sidecar_wgview_set (&s_Wg); run (2);
		shot ("mach-wavetable-edit");
		s_nCmds = 0;
		tap (ed_x, WG_KY);
		EXPECT (s_nCmds >= 1 && sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown () && sidecar_wgview_page_list () == WGV_SAMPLES);
		EXPECT (!strcmp (sidecar_wgview_message (), o ? "PICK A SAMPLE: TAP ITS ROW, EDIT" : "Pick a sample: tap its row, Edit"));
		WG_PUSH (); shot ("pool-page-pick-sample");
		sidecar_ui_show (7); run (2);
		wg_fake (WGV_WAVETABLE, 0.3); sidecar_wgview_set (&s_Wg); run (2);
	}
	s_WgRamKnown = 0;			// (until the shared cache counts: "RAM -")
	tap (WG_SRC_X, WG_KY); WG_PUSH (); shot ("pool-tables");
	EXPECT (sidecar_wgview_list_shown () && sidecar_wgview_list_id () == WGV_TABLES);
	tap (wlx, WG_ROW (1 + 2)); EXPECT_CMD ("wt=pick:2:A002");
	// TBLB: the middle key picks the table the rows set: B (P13: wt=pickb), the title says so; silent at MIX 0 (FM / RM aside) is
	// said, briefly; the next pick of B at an open MIX, a pick of A and the key again take it back; every open starts on A
	EXPECT (!sidecar_wgview_target_b ());
	tap (WG_MID_X, WG_KY); WG_PUSH ();
	EXPECT (sidecar_wgview_target_b ());
	s_WgSelB = 3, s_WgMix0 = 0; WG_PUSH (); shot ("pool-tables-b");
	tap (wlx, WG_ROW (1 + 4)); EXPECT_CMD ("wt=pickb:2:A004");
	EXPECT (!sidecar_wgview_message ()[0]);
	s_WgMix0 = 1; WG_PUSH ();
	tap (wlx, WG_ROW (1 + 5)); EXPECT_CMD ("wt=pickb:2:A005");
	EXPECT (!strcmp (sidecar_wgview_message (), o ? "B IS SILENT: MIX IS 0" : "B is silent: MIX is 0"));
	shot ("pool-tables-b-silent");
	tap (wlx, WG_ROW (1 + 0)); EXPECT_CMD ("wt=pickb:2:A000");			// B off: nothing to say
	EXPECT (!sidecar_wgview_message ()[0]);
	tap (wlx, WG_ROW (1 + 5)); EXPECT (!strcmp (sidecar_wgview_message (), o ? "B IS SILENT: MIX IS 0" : "B is silent: MIX is 0"));
	tap (WG_MID_X, WG_KY); WG_PUSH ();						// back to A
	EXPECT (!sidecar_wgview_target_b () && !sidecar_wgview_message ()[0]);
	tap (wlx, WG_ROW (1 + 2)); EXPECT_CMD ("wt=pick:2:A002");
	tap (WG_MID_X, WG_KY); WG_PUSH ();
	EXPECT (sidecar_wgview_target_b ());
	sidecar_wgview_pool_page (1);		// the POOL page by another route than DONE: no table B there (the TABLES tab marks A)
	EXPECT (!sidecar_wgview_target_b ());
	sidecar_wgview_pool_page (0); WG_PUSH ();
	tap (WG_SRC_X, WG_KY); WG_PUSH ();	// the list again, on A, and B once more
	EXPECT (sidecar_wgview_list_shown () && !sidecar_wgview_target_b ());
	tap (WG_MID_X, WG_KY); WG_PUSH ();
	EXPECT (sidecar_wgview_target_b ());
	tap (WG_SRC_X, WG_KY);			// DONE
	tap (WG_SRC_X, WG_KY); WG_PUSH ();	// POOL again: A
	EXPECT (sidecar_wgview_list_shown () && !sidecar_wgview_target_b ());
	s_WgSelB = -1, s_WgMix0 = 0; WG_PUSH ();
	tap (wlx, WG_ROW (1 + 8)); WG_PUSH ();
	EXPECT (sidecar_wgview_target () == 8);
	tap (wlx, WG_ROW (5)); EXPECT_CMD ("pool=wt:assign:A008:3");
	tap (WG_SRC_X, WG_KY);			// BACK
	s_nCmds = 0;
	hold (wlx, WG_ROW (1 + 1)); WG_PUSH ();
	EXPECT (sidecar_wgview_asking () == 2);
	tap (wlx, WG_BOX (3, 1));		// UNLOAD
	EXPECT (sidecar_wgview_asking () == 1 && !strcmp (sidecar_wgview_message (), "ROW A001 USED BY 1 SOUND."));
	tap (no_x, WG_BOX (3, 2));
	EXPECT (s_nCmds == 0 && !sidecar_wgview_asking ());
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (!sidecar_wgview_list_shown ());
	s_WgRamKnown = 1;

	// touch2: MULTISAMPLE in the machine view: its zones and keys; POOL its INSTRUMENTS: a pick, tab D, an empty row's
	// FILES (.sfz, no FILL), an instrument into it
	wg_fake (WGV_MULTISAMPLE, 0.3); sidecar_wgview_set (&s_Wg); run (2); shot ("mach-multisample");
	EXPECT (sidecar_wgview_shown ());
	tap (WG_SRC_X, WG_KY); WG_PUSH (); shot ("pool-instr");
	EXPECT (sidecar_wgview_list_shown () && sidecar_wgview_list_id () == WGV_INSTR);
	tap (wlx, WG_ROW (1 + 2)); EXPECT_CMD ("msamp=pick:2:A002");
	tap (tx0 + 3 * tw + tw / 2, WG_ROW (0)); EXPECT_CMD ("pool=inst:shown:D");
	WG_PUSH (); shot ("pool-instr-bank-d");
	tap (wlx, WG_ROW (1 + 2)); EXPECT_CMD ("msamp=pick:2:D002");
	tap (wlx, WG_ROW (1 + 5)); WG_PUSH ();
	EXPECT (sidecar_wgview_lib_shown () && sidecar_wgview_target () == 3 * 128 + 5);
	shot ("pool-instr-files");
	tap (wlx, WG_ROW (5)); EXPECT_CMD ("pool=inst:assign:D005:3");				// Rhodes Mk1.sfz into D005
	tap (WG_SRC_X, WG_KY);			// BACK
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (!sidecar_wgview_list_shown ());
	sidecar_wgview_set_bank (0);

	// the POOL page: ALL POOLS (the machine view's middle key); the three lists' tabs, a tap on a row its menu
	// (ACCEPT on a changed file), DONE back to the machine view; Settings > Pool opens it too
	tap (WG_MID_X, WG_KY);
	EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown () && sidecar_wgview_list_shown ());
	sidecar_wgview_set_page_list (WGV_SAMPLES);
	WG_PUSH (); shot ("pool-page-samples");
	tap (tx0 + tw5 + tw5 / 2, WG_ROW (0)); WG_PUSH ();
	EXPECT (sidecar_wgview_page_list () == WGV_TABLES && sidecar_wgview_list_id () == WGV_TABLES);
	shot ("pool-page-tables");
	tap (tx0 + 2 * tw5 + tw5 / 2, WG_ROW (0)); WG_PUSH ();
	EXPECT (sidecar_wgview_page_list () == WGV_INSTR);
	shot ("pool-page-instr");
	tap (wlx, WG_ROW (2 + 1)); EXPECT (sidecar_wgview_asking () == 2);
	tap (wlx, WG_BOX (4, 3)); EXPECT (!sidecar_wgview_asking ());				// CANCEL
	tap (tx0 + tw5 / 2, WG_ROW (0)); WG_PUSH ();
	tap (wlx, WG_ROW (2 + 5)); WG_PUSH ();							// A005: changed
	EXPECT (sidecar_wgview_asking () == 2);
	shot ("pool-page-menu");
	tap (wlx, WG_BOX (6, 4)); EXPECT_CMD ("pool=smp:accept:A005");		// (a sample's menu: EDIT REPLACE UNLOAD ACCEPT CANCEL)
	tap (WG_SRC_X, WG_KY);			// DONE: back
	EXPECT (sidecar_ui_page () == 7 && !sidecar_wgview_pool_page_shown () && !sidecar_wgview_list_shown ());
	// the machine view's ALL POOLS, its JUNO tab (the PRESET page), DONE: back to the machine view (page 7), not POOL
	tap (WG_MID_X, WG_KY); EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown ());
	tap (tx0 + 3 * tw5 + tw5 / 2, WG_ROW (0)); EXPECT (sidecar_ui_page () == 18);
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (sidecar_ui_page () == 7);
	sidecar_ui_show (1); run (2);
	tap_raw (SUBX (6, 3), SUBY);	// Settings > Pool (the 4th of 6 pills)
	EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown ());
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (sidecar_ui_page () == 1);
	// Settings > Pool, its JUNO tab (the PRESET page), Settings > Pool again from there: POOL, and DONE back to Settings
	// (0.5.1 stored PRESET as where POOL came from: page_sec recursed without end, and DONE looped POOL <-> PRESET)
	tap_raw (SUBX (6, 3), SUBY); EXPECT (sidecar_ui_page () == 15);
	tap (tx0 + 3 * tw5 + tw5 / 2, WG_ROW (0)); EXPECT (sidecar_ui_page () == 18);
	tap_raw (SUBX (6, 3), SUBY); EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown ());
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (sidecar_ui_page () == 1);
	// from the machine view: ALL POOLS, JUNO, then the SET key (its remembered pill is Pool: POOL again from PRESET), DONE:
	// the machine view (page 7), not PRESET. (Settings > Pool first: it makes Pool the SET key's remembered pill)
	tap_raw (SUBX (6, 3), SUBY); EXPECT (sidecar_ui_page () == 15);
	sidecar_ui_show (7); run (2);
	tap (WG_MID_X, WG_KY); EXPECT (sidecar_ui_page () == 15);
	tap (tx0 + 3 * tw5 + tw5 / 2, WG_ROW (0)); EXPECT (sidecar_ui_page () == 18);
	if (o) tap_raw (NAV_X, NAV_Y (7)); else tap_raw (CNAV_X (7), CNAV_Y (7));
	EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown ());
	tap (WG_SRC_X, WG_KY);			// DONE
	EXPECT (sidecar_ui_page () == 7);
	// the POOL page's JUNO / 6-OP tabs: the preset pages (sidecar_preset.c). The JUNO list, a slot's menu (REPLACE), the
	// index aimed at the slot (a folder, a preset: preset=juno:set:..), an empty slot, ADD (the first empty slot), the FM
	// list holding the built-ins, the index scrolled, the tabs back to the pool lists
	{
		const int pup_x = o ? (LU (84) + DX / 4) / 2 : LU (16) + (LU (140) + DX / 4) / 2;
		s_PreBuiltFm = 1, s_PreMsg[0] = 0;
		sidecar_ui_show (15); run (2);
		sidecar_wgview_set_page_list (WGV_SAMPLES);
		PRE_PUSH (); WG_PUSH ();
		shot ("pool-page-5tabs");
		tap (tx0 + 3 * tw5 + tw5 / 2, WG_ROW (0));
		EXPECT (sidecar_ui_page () == 18 && sidecar_preset_shown () && sidecar_preset_kind () == 0 && !sidecar_wgview_shown ());
		PRE_PUSH (); shot ("preset-juno");
		tap (wlx, WG_ROW (1 + 2)); PRE_PUSH ();							// an empty slot: the index aimed at it
		EXPECT (sidecar_preset_index_shown () && sidecar_preset_folder () == 0);
		shot ("preset-index-slot");
		tap (wlx, WG_ROW (1)); PRE_PUSH ();							// the path row at a root: back to the list
		EXPECT (!sidecar_preset_index_shown () && sidecar_preset_folder () == -1);
		tap (wlx, WG_ROW (1 + 3)); PRE_PUSH ();							// a filled slot: its menu
		EXPECT (sidecar_preset_asking ());
		shot ("preset-menu");
		tap (wlx, WG_BOX (3, 1)); PRE_PUSH ();							// REPLACE
		EXPECT (sidecar_preset_index_shown () && !sidecar_preset_asking ());
		tap (wlx, WG_ROW (2)); PRE_PUSH ();							// FACTORY/
		EXPECT (sidecar_preset_folder () == 2);
		shot ("preset-replace-index");
		tap (wlx, WG_ROW (2 + 2)); EXPECT_CMD ("preset=juno:set:7:2:2:3");			// A13 Bass 1 into slot 3
		EXPECT (!sidecar_preset_index_shown ());
		snprintf (s_PreMsg, sizeof s_PreMsg, "JUNO 003 SET"); s_nPreSeq++;
		PRE_PUSH (); shot ("preset-replaced");
		tap (WG_MID_X, WG_KY); PRE_PUSH ();							// ADD: the first empty slot
		EXPECT (sidecar_preset_index_shown () && sidecar_preset_folder () == 0);
		tap (wlx, WG_ROW (2 + 1)); PRE_PUSH ();							// Pads/
		EXPECT (sidecar_preset_folder () == 3);
		tap (wlx, WG_ROW (2 + 1)); EXPECT_CMD ("preset=juno:add:7:3:1");			// stays in the index
		EXPECT (sidecar_preset_index_shown ());
		snprintf (s_PreMsg, sizeof s_PreMsg, "JUNO 002 ADDED"); s_nPreSeq++;
		PRE_PUSH (); shot ("preset-added");
		tap (wlx, WG_ROW (1)); PRE_PUSH ();							// up to JUNO
		EXPECT (sidecar_preset_folder () == 0);
		tap (WG_SRC_X, WG_KY); PRE_PUSH ();							// BACK to the list
		EXPECT (!sidecar_preset_index_shown ());
		tap (tx0 + 4 * tw5 + tw5 / 2, WG_ROW (0)); PRE_PUSH ();				// 6-OP FM: the built-ins
		EXPECT (sidecar_preset_kind () == 1);
		shot ("preset-fm-builtin");
		tap (WG_MID_X, WG_KY); PRE_PUSH ();
		EXPECT (sidecar_preset_folder () == 1);
		tap (wlx, WG_ROW (2)); PRE_PUSH ();							// ROM/
		EXPECT (sidecar_preset_folder () == 4);
		tap (WG_DN_X, WG_KY); PRE_PUSH ();							// DOWN: a page on
		EXPECT (sidecar_preset_first () > 0);
		shot ("preset-fm-rom-scrolled");
		tap (wlx, WG_ROW (2 + 1)); { char want[48]; snprintf (want, sizeof want, "preset=fm:add:7:4:%d", sidecar_preset_first () + 1); EXPECT_CMD (want); }
		s_nPreSeq++; snprintf (s_PreMsg, sizeof s_PreMsg, "FM 096 ADDED"); PRE_PUSH ();
		tap (tx0 + tw5 / 2, WG_ROW (0));							// SAMPLES: the POOL page again
		EXPECT (sidecar_ui_page () == 15 && sidecar_wgview_pool_page_shown () && sidecar_wgview_page_list () == WGV_SAMPLES);
		sidecar_ui_show (15); run (2);
		tap (tx0 + 3 * tw5 + tw5 / 2, WG_ROW (0)); EXPECT (sidecar_ui_page () == 18);
		tap (WG_SRC_X, WG_KY);									// DONE: where the POOL page came from
		EXPECT (sidecar_ui_page () != 18 && !sidecar_preset_shown ());
		(void) pup_x;
	}
#undef WG_ROW
#undef WG_BOX
#undef WG_MSG
#undef WG_PUSH
	}
	// a computer started its USB network late (net=auto): the question, RESTART / NOT NOW (it is the caller that restarts), a failed drain's message
	{
		int hx, hy;
		sidecar_ui_show (0); run (2);
		EXPECT (!sidecar_ui_latenet_hit (1, &hx, &hy) && sidecar_ui_latenet_take () == 0);
		sidecar_ui_latenet (1); run (2);
		shot ("late-computer");
		EXPECT (sidecar_ui_latenet_hit (0, &hx, &hy));
		tap_raw (hx, hy); run (2);
		EXPECT (sidecar_ui_latenet_take () == 1 && sidecar_ui_latenet_take () == 0);
		EXPECT (sidecar_ui_latenet_hit (1, &hx, &hy));
		tap_raw (hx, hy); run (2);
		EXPECT (sidecar_ui_latenet_take () == 2);
		sidecar_ui_latenet (2); run (2);					// the drain did not settle: RESTART is gone, OK stays
		shot ("late-computer-failed");
		EXPECT (!sidecar_ui_latenet_hit (0, &hx, &hy) && sidecar_ui_latenet_hit (1, &hx, &hy));
		tap_raw (hx, hy); run (2);
		EXPECT (sidecar_ui_latenet_take () == 2);
		sidecar_ui_latenet (0); run (2);
		EXPECT (!sidecar_ui_latenet_hit (1, &hx, &hy));
	}
	// the remote app's pages it does not feed (sidecar_ui_page_enable): the panel over the content, the key / pill greyed; fed again, all as before
	{
		sidecar_ui_show (4); run (2);
		EXPECT (!sidecar_ui_nofeed_shown () && sidecar_ui_nav_enabled (1));			// every page is fed by default (the Pi's own screen)
		sidecar_ui_page_enable (4, 0); run (2);						// SEQ: no page of its section is fed
		EXPECT (sidecar_ui_nofeed_shown () && !sidecar_ui_nav_enabled (1) && sidecar_ui_nav_enabled (2));
		shot ("not-in-remote");
		tap (30, 100); run (2);									// a touch on the panel does not reach the page under it
		EXPECT (sidecar_ui_nofeed_shown () && sidecar_ui_page () == 4);
		sidecar_ui_show (5); run (2);								// PAT is fed
		EXPECT (!sidecar_ui_nofeed_shown ());
		sidecar_ui_page_enable (8, 0); sidecar_ui_page_enable (9, 0); sidecar_ui_page_enable (12, 0); sidecar_ui_page_enable (7, 0); run (2);
		EXPECT (sidecar_ui_nav_enabled (3) == 0);						// VOICE's section: Sound, Insert, Source, Scope all off
		sidecar_ui_page_enable (9, 1); run (2);
		EXPECT (sidecar_ui_nav_enabled (3));							// one page fed: the section's key is on
		sidecar_ui_show (8); run (2);
		EXPECT (sidecar_ui_nofeed_shown ());
		sidecar_ui_page_enable (4, 1); sidecar_ui_page_enable (8, 1); sidecar_ui_page_enable (12, 1); sidecar_ui_page_enable (7, 1); run (2);
		EXPECT (!sidecar_ui_nofeed_shown () && sidecar_ui_nav_enabled (1) && sidecar_ui_nav_enabled (3));
		sidecar_ui_show (0); run (2);
	}
	// T3: every HOME voice opens VOICE for its first track (voicepick=); GRANULAR / WAVETABLE their machine view
	sidecar_ui_show (0); run (2);
	{
		const char *keep = s_St.track[6];
#define CYCB_TAP() do { int hx_, hy_; EXPECT (sidecar_ui_home_hit (sidecar_ui_voice_pos (6), &hx_, &hy_)); tap (hx_, hy_); } while (0)	// CY/CB: voice 6
		s_St.track[6] = "MULTI"; sidecar_ui_status (&s_St); run (2);
		CYCB_TAP ();
		EXPECT (sidecar_ui_page () == 8); EXPECT_CMD ("voicepick=10");
		sidecar_ui_show (0); run (2);
		s_St.track[6] = "GRAN"; sidecar_ui_status (&s_St); run (2);
		CYCB_TAP ();
		EXPECT (sidecar_ui_page () == 7);
#undef CYCB_TAP
		s_St.track[6] = keep; sidecar_ui_status (&s_St);
	}

	// rui1036b: the VOICE page (UP + T4 on the Rytm) for an analog BD, a MULTISAMPLE and a 6-OP FM track
	sidecar_ui_show (8);
	EXPECT (sidecar_voice_shown () && sidecar_ui_page () == 8);
	EXPECT (sidecar_ui_nav_lit () == 3);				// N2: VOICE lights VOICE, never SET
	voice_fake (0); sidecar_voice_set (&s_Vv); run (2); shot ("voice-analog-bd");
	if (o) tap (LU (150), LU (91) + DY / 6 + LU (40)); else tap (LU (440) + DX / 4 + LU (100), LU (52) + LU (84) + LU (100));	// touch2: the scope -> SCOPE (TRACK)
	EXPECT (sidecar_ui_page () == 12 && sidecar_scope_source () == -1);
	sidecar_ui_show (8);
	voice_fake (1); sidecar_voice_set (&s_Vv); run (2); shot ("voice-multisample");
	if (o) tap (LU (150), LU (87) + DY / 6 + LU (50)); else tap (LU (440) + DX / 4 + LU (100), LU (52) + LU (80) + LU (100));	// touch2: the panel -> the banks
	EXPECT (sidecar_ui_page () == 7 && sidecar_wgview_list_shown ());
	sidecar_wgview_list (0);
	sidecar_ui_show (8);
	voice_fake (2); sidecar_voice_set (&s_Vv); run (2); shot ("voice-fm");
	{	// touch controls (VOICE: the Rytm's own knobs, "vparam=T:K:V"): the 6-OP FM track (3) but its PAT (a list); no double tap
		s_nVoiceKind = 2;
		EDIT_CHECK_W (sidecar_voice_knob (1), 1, 64, -1, 127, 64, "vparam=2:1:%d", push_voice);	// TIM
		EDIT_CHECK_W (sidecar_voice_knob (6), 1, 100, -1, 127, 100, "vparam=2:6:%d", push_voice);	// LEV
		EDIT_NONE (sidecar_voice_knob (0));
		{	// the text: a write that never lands (no answer, a dropped one, an old Rytm) leaves nothing of the finger's number once the hold ends
			lv_obj_t *k = (lv_obj_t *) sidecar_voice_knob (1);
			lv_area_t c;
			lv_obj_get_coords (k, &c);
			const int hx = (c.x1 + c.x2) / 2, hy = (c.y1 + c.y2) / 2;
			run (60);
			int c0 = s_nCmds;
			drag_hold_raw (hx, hy, hx, hy - TH->touch, 6), lift ();
			EXPECT (s_nCmds > c0 + 1 && strcmp (th_meter_value_get (k), "64") != 0 && th_meter_get (k) != 64);	// the finger's, while held
			EXPECT (strlen (s_Hist[c0 & 255]) > 2 && !strcmp (s_Hist[c0 & 255] + strlen (s_Hist[c0 & 255]) - 2, ":0"));	// (a live line says so)
			EXPECT (strchr (s_Last, ':') && strcmp (s_Last + strlen (s_Last) - 2, ":0") != 0);					// (the lift's does not)
			// an echo of the writes on their way (two other values in a row) gives the Rytm the say again, once; one is not enough
			voice_fake (2); s_Vv.val[1] = 40; s_Vv.gen = ++s_nPushGen; sidecar_voice_set (&s_Vv); run (2);
			EXPECT (th_meter_editing (k));
			voice_fake (2); s_Vv.val[1] = 41; s_Vv.gen = ++s_nPushGen; sidecar_voice_set (&s_Vv); run (2);
			EXPECT (!th_meter_editing (k) && th_meter_get (k) == 41 && !strcmp (th_meter_value_get (k), "41"));
			voice_fake (2); sidecar_voice_set (&s_Vv); run (2);
			run (60);
			drag_hold_raw (hx, hy, hx, hy - TH->touch, 6), lift ();		// again, and the Rytm never moves
			run (70);
			EXPECT (!th_meter_editing (k) && th_meter_get (k) == 64 && !strcmp (th_meter_value_get (k), "64"));
		}
		// the Rytm refuses (or the steps are locked): the knob is the Rytm's again, and it says so, for a moment
		{
			lv_area_t c;
			lv_obj_get_coords ((lv_obj_t *) sidecar_voice_knob (3), &c);
			const int hx = (c.x1 + c.x2) / 2, hy = (c.y1 + c.y2) / 2;
			for (int kind = 1; kind <= 2; kind++)
			{
				voice_fake (2); s_Vv.gen = ++s_nPushGen; sidecar_voice_set (&s_Vv); run (2);		// (a clean view: the knob's next write cleared the old failure)
				run (60);
				drag_hold_raw (hx, hy, hx, hy + TH->touch, 6), lift ();
				EXPECT (th_meter_editing ((lv_obj_t *) sidecar_voice_knob (3)) && th_meter_get ((lv_obj_t *) sidecar_voice_knob (3)) != 127);
				voice_fake (2);
				s_Vv.wfail = 1 << 3, s_Vv.wkind = (unsigned char) kind, s_Vv.wgen = (unsigned) ++s_nPushGen, s_Vv.gen = ++s_nPushGen;
				sidecar_voice_set (&s_Vv); run (2);
				EXPECT (!th_meter_editing ((lv_obj_t *) sidecar_voice_knob (3)) && th_meter_get ((lv_obj_t *) sidecar_voice_knob (3)) == 127);
				EXPECT (text_shown (lv_screen_active (), kind == 1 ? "LOCKED ON THE HELD STEPS" : "THE RYTM REFUSED"));
				run (80);
				EXPECT (!text_shown (lv_screen_active (), "REFUSED") && !text_shown (lv_screen_active (), "LOCKED ON") && text_shown (lv_screen_active (), "UP + T4: HERE"));
			}
			// another track while a finger is down: the drag is dropped, the knob shows the new track's value
			run (60);
			drag_hold_raw (hx, hy, hx, hy + TH->touch, 6);
			EXPECT (th_meter_editing ((lv_obj_t *) sidecar_voice_knob (3)));
			voice_fake (2);
			s_Vv.track = 7, s_Vv.val[3] = 9, s_Vv.gen = ++s_nPushGen;
			sidecar_voice_set (&s_Vv); run (2);
			EXPECT (!th_meter_editing ((lv_obj_t *) sidecar_voice_knob (3)) && th_meter_get ((lv_obj_t *) sidecar_voice_knob (3)) == 9);
			lift ();
			voice_fake (2); sidecar_voice_set (&s_Vv); run (2);
		}
	}
	voice_fake (3); sidecar_voice_set (&s_Vv); run (2); shot ("voice-daisy-modal");
	s_Vv.dz_model = 1, s_Vv.val[0] = 64, s_Vv.gen++; sidecar_voice_set (&s_Vv); run (2); shot ("voice-daisy-string");
	s_Vv.dz_model = 2, s_Vv.val[0] = 120, s_Vv.gen++; sidecar_voice_set (&s_Vv); run (2); shot ("voice-daisy-reson");
	voice_fake (4); sidecar_voice_set (&s_Vv); run (2); shot ("voice-sampler");
	s_nVoiceKind = 4;			// SAMPLER: its page's knobs map to the Rytm's P12 / P13 .. (the line carries that index); SMP (P11) is banked
	EDIT_CHECK_W (sidecar_voice_knob (1), 1, 0, -1, 127, 0, "vparam=4:12:%d", push_voice);	// STRT
	EDIT_CHECK_W (sidecar_voice_knob (6), 1, 0, -1, 127, 0, "vparam=4:14:%d", push_voice);	// LOOP
	EDIT_NONE (sidecar_voice_knob (0));
	voice_fake (0); sidecar_voice_set (&s_Vv); run (2);		// back to an analog track: the scope again
	EXPECT (s_Vv.mode == SVV_PLAIN);
	EDIT_NONE (sidecar_voice_knob (1));	// an analog track: its knobs are the Rytm's alone
	{
		sidecar_voice_view_t z;
		memset (&z, 0, sizeof z);
		z.gen = 7; z.track = -1;
		sidecar_voice_set (&z); run (2); shot ("voice-waiting");
	}
	// fx-ins: the INSERT FX page (Settings > Send FX > Inserts; UP + T5): voice 3's chain and sends, a TYPE tap
	sidecar_ui_show (9);
	EXPECT (sidecar_insfx_shown () && sidecar_ui_page () == 9);
	ins_fake (11); sidecar_insfx_set (&s_Iv); run (2); shot ("insfx-voice");
	ins_fake (12); ins_slot (2, 1, 10, "SAT", "CHAR", "DRV", "TONE", "MIX", "TAPE", "+8DB", "FLAT", "127", 0, 40, 64, 127);
	sidecar_insfx_set (&s_Iv); run (2); shot ("insfx-sat");
	ins_fake (13); sidecar_insfx_set (&s_Iv); run (2);
	// the MASTER FX page: both SRCs, the commands its keys send
	{
		int pg = sidecar_ui_page ();
		sidecar_ui_show (19);
		EXPECT (sidecar_mfx_shown ());
		mfx_fake (0); sidecar_mfx_set (&s_Mv); run (2); shot ("mfx-ret");
		mfx_fake (1); sidecar_mfx_set (&s_Mv); run (2); shot ("mfx-mix");
		mfx_fake (1); s_Mv.rep = 1; s_Mv.onmain = 3; s_Mv.gen++; sidecar_mfx_set (&s_Mv); run (2); shot ("mfx-through");
		mfx_fake (0); s_Mv.gen = ++s_nPushGen; sidecar_mfx_set (&s_Mv); run (2);		// touch controls: the bars, MIX and LEV
		EDIT_CHECK (sidecar_mfx_obj (0), 0, 30, 40, 127, 30, "mfx=1:p1:%d", push_mfx);		// slot 1 (COMP) P1
		EDIT_CHECK (sidecar_mfx_obj (8 + 3), 0, 105, 70, 127, 105, "mfx=2:p4:%d", push_mfx);	// slot 2 (AMP) P4
		EDIT_CHECK (sidecar_mfx_obj (16 + 4), 0, 64, 40, 127, 64, "mfx=3:mix:%d", push_mfx);	// slot 3 (SPACE, a send TYPE): MIX
		EDIT_CHECK (sidecar_mfx_obj (100), 0, 100, 100, 127, 100, "mfx=lev:%d", push_mfx);		// LEV
		EDIT_NONE (sidecar_mfx_obj (4));				// slot 1 (COMP): no MIX
		EDIT_NONE (sidecar_mfx_obj (24));				// slot 4 (OFF): no bars to move
		EDIT_NONE (sidecar_mfx_obj (24 + 4));
		sidecar_ui_show (pg);
		EXPECT (!sidecar_mfx_shown ());
	}
	// 0.5.0: the MORPH page - no locks, a full list, the last rows, the engine not there yet
	{
		int pg = sidecar_ui_page ();
		sidecar_ui_show (20);
		EXPECT (sidecar_morph_shown () && !sidecar_mfx_shown ());
		morph_fake (0, 0); sidecar_morph_set (&s_Mo); run (2); shot ("morph-empty");
		morph_fake (24, 0); sidecar_morph_set (&s_Mo); run (2); shot ("morph-locks");
		morph_fake (24, 16); s_Mo.edit = 1; s_Mo.gen++; sidecar_morph_set (&s_Mo); run (2); shot ("morph-edit");
		{	// a double tap puts the crossfader to A (its drag stays absolute); a tap alone sets what it always did
			lv_area_t c;
			lv_obj_get_coords ((lv_obj_t *) sidecar_morph_fader (), &c);
			const int hx = (c.x1 + c.x2) / 2, hy = (c.y1 + c.y2) / 2;
			run (12);
			int c0 = s_nCmds;
			double_tap_raw (hx, hy);
			EXPECT_CMD ("scene=fader:0");
			EXPECT (s_nCmds - c0 <= 4 && strncmp (s_Hist[c0 & 255], "scene=fader:", 12) == 0);	// (the first tap's own lines, then the reset)
			EXPECT (th_slider_get ((lv_obj_t *) sidecar_morph_fader ()) == 0);
			run (12);
			c0 = s_nCmds;
			tap_raw (hx, hy), run (12), tap_raw (hx, hy), run (12);				// two taps a second apart: no reset
			EXPECT (strcmp (s_Last, "scene=fader:0") && s_nCmds > c0);
			// a tap, then within 450 ms a press that drags: a drag as any (its lines follow the finger), no reset
			run (12);
			c0 = s_nCmds;
			tap_raw (hx, hy), run (1);
			drag_hold_raw (hx, hy, hx + 60, hy, 6), lift ();
			int reset = 0;
			for (int i = c0; i < s_nCmds; i++) reset += !strcmp (s_Hist[i & 255], "scene=fader:0");
			EXPECT (!reset && s_nCmds - c0 >= 4 && strncmp (s_Last, "scene=fader:", 12) == 0 && strcmp (s_Last, "scene=fader:0"));
			// a tap, then a long press that stays put: no reset either
			run (12);
			c0 = s_nCmds;
			tap_raw (hx, hy), run (1);
			drag_hold_raw (hx, hy, hx, hy, 1), run (15), lift ();
			reset = 0;
			for (int i = c0; i < s_nCmds; i++) reset += !strcmp (s_Hist[i & 255], "scene=fader:0");
			EXPECT (!reset);
		}
		// v2 Task 5: ADD - RT1 FLTR FRQ at 87 into the selected scene (3); RFX REVERB RDEC; BACK closes
		morph_fake (5, 0); sidecar_morph_set (&s_Mo); run (2);
		sidecar_morph_add (1);
		EXPECT (!strcmp (sidecar_morph_add_name (), "RT1:P1"));
		sidecar_morph_add_step (1, 2); sidecar_morph_add_step (2, 5); sidecar_morph_add_value (87); run (2);
		shot ("morph-add");
		sidecar_morph_add_go ();
		EXPECT_CMD ("scene=lock:3:RT1:FRQ:87");
		sidecar_morph_add_step (0, -9); sidecar_morph_add_step (1, 1); sidecar_morph_add_step (2, 1); sidecar_morph_add_value (100);
		sidecar_morph_add_go ();
		EXPECT_CMD ("scene=lock:3:RFX:RDEC:100");
		sidecar_morph_add_step (0, 1); sidecar_morph_add_step (1, 1);
		EXPECT (!strcmp (sidecar_morph_add_name (), "V1:P9"));
		sidecar_morph_add (0);
		EXPECT (!sidecar_morph_add_name ()[0]);
		// v2 Task 7: a lock whose target is gone (faint, "?"); DEL offers UNDO, which locks it back at its value
		morph_fake (5, 0); s_Mo.row[2].stale = 1; s_Mo.gen++; sidecar_morph_set (&s_Mo); run (2);
		shot ("morph-stale");
		sidecar_morph_del (0); run (2);
		EXPECT_CMD ("scene=unlock:3:V1:P3");
		EXPECT (sidecar_morph_undo_shown ());
		shot ("morph-undo");
		sidecar_morph_undo ();
		EXPECT_CMD ("scene=lock:3:V1:P3:64");
		EXPECT (!sidecar_morph_undo_shown ());
		sidecar_ui_show (pg);
		EXPECT (!sidecar_morph_shown ());
	}
	// 0.5.0: the LOOP page - no grabs, a loop playing with eight grabs (the longest texts), the looper off, the engine not there yet
	{
		int pg = sidecar_ui_page ();
		sidecar_ui_show (25);
		EXPECT (sidecar_loop_shown () && !sidecar_morph_shown ());
		loop_fake (0, 0); sidecar_loop_set (&s_Lo); run (2); shot ("loop-empty");
		loop_fake (8, 2); sidecar_loop_set (&s_Lo); run (2); shot ("loop-playing");
		loop_fake (3, 1); s_Lo.pending = 1; s_Lo.gen++; sidecar_loop_set (&s_Lo); run (2); shot ("loop-resample");
		loop_fake (0, 0); s_Lo.on = 0; s_Lo.gen++; sidecar_loop_set (&s_Lo); run (2); shot ("loop-off");
		loop_fake (8, 2); sidecar_loop_set (&s_Lo); run (2);
		{	// the controls send the console's lines (a pad plays and shows; STOP fades, CUT cuts at once; sliders absolute)
			int x, y, c0 = s_nCmds;
			EXPECT (sidecar_loop_key (0, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=grab:0.5");
			EXPECT (sidecar_loop_key (5, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=grab:16");
			EXPECT (sidecar_loop_key (6, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=stop");
			EXPECT (sidecar_loop_key (7, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=stop:cut:now");
			EXPECT (sidecar_loop_key (10, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=play:#102"); EXPECT (sidecar_loop_sel () == 2);
			EXPECT (sidecar_loop_key (16, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=keep:#102");
			EXPECT (sidecar_loop_key (17, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=fade:2");	// FADE 1 -> 2 (the fake: index 2 = 1 bar, next 2)
			EXPECT (sidecar_loop_key (18, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=mute:off");	// (the fake has it on)
			EXPECT (sidecar_loop_key (19, &x, &y)); tap_raw (x, y); EXPECT_CMD ("loop=live:off");	// (the fake has PERF 12: past it is off)
			EXPECT (s_nCmds - c0 == 9);
		}
		sidecar_ui_show (pg);
		EXPECT (!sidecar_loop_shown ());
	}
	// 0.5.0: the XY page - empty, a drag (xy=pos while it moves, the last position and xy=up at the release), HOLD's
	// held position, SETUP with every slot, the picker (its SET, NONE), the engine not there yet
	{
		int pg = sidecar_ui_page ();
		sidecar_ui_show (22);
		EXPECT (sidecar_xy_shown () && !sidecar_morph_shown () && !sidecar_xy_setup_shown ());
		xy_fake (0, 0, 0); sidecar_xy_set (&s_Xy); run (2); shot ("xy-empty");
		xy_fake (4, 0, 0); sidecar_xy_set (&s_Xy); run (2);
		int px, py, pw, ph;
		sidecar_xy_pad_area (&px, &py, &pw, &ph);
		const int pyo = o ? py : py - LU (58);			// (SIDECAR: the area has the page's origin in it, OLED: not the title bar's shift; tap and drag add it)
		int n0 = s_nXyPos, c0 = s_nCmds;
		drag (px + pw / 8, pyo + ph * 7 / 8, px + pw * 3 / 4, pyo + ph / 4, 10, "xy-drag");
		EXPECT (s_nXyPos - n0 >= 3);		// live while dragging
		EXPECT_CMD ("xy=up");			// the release last, the final position before it
		EXPECT (s_nCmds - c0 == s_nXyPos - n0 + 1);
		// review M2: a move the 30 ms throttle skipped goes out while the finger rests there (indev read every 5 ms)
		{
			lv_timer_t *rt = lv_indev_get_read_timer (lv_indev_get_next (NULL));
			lv_timer_set_period (rt, 5);
			s_Px = sx (px + pw / 4); s_Py = sy (py + ph / 2 + (o ? LU (25) : 0)); s_Down = 1;
			for (int i = 0; i < 4; i++) { lv_tick_inc (10); lv_timer_handler (); }	// pressed, 40 ms on
			s_Px = sx (px + pw / 2);  lv_tick_inc (10); lv_timer_handler ();		// sent (>= 30 ms)
			s_Px = sx (px + pw * 3 / 4); lv_tick_inc (10); lv_timer_handler ();	// 10 ms later: held back
			for (int i = 0; i < 6; i++) { lv_tick_inc (10); lv_timer_handler (); }	// the finger rests
			char rest[40];
			snprintf (rest, sizeof rest, "%s", s_XyPos);
			s_Down = 0; lv_tick_inc (10); lv_timer_handler ();
			EXPECT_CMD ("xy=up");
			EXPECT (strcmp (rest, s_XyPos) == 0);	// the release's last position was already out while resting
			lv_timer_set_period (rt, LV_DEF_REFR_PERIOD);
			run (2);
		}
		// review M3: the engine still touching with no finger down (an xy=up dropped): xy=up again, now and then
		{
			int u0 = s_nXyUp;
			xy_fake (4, 0, 0); s_Xy.touching = 1; s_Xy.active = 1; s_Xy.gen = 0x5150;
			for (int i = 0; i < 20; i++) { sidecar_xy_set (&s_Xy); lv_tick_inc (50); lv_timer_handler (); }	// 1 s
			EXPECT (s_nXyUp - u0 >= 1 && s_nXyUp - u0 <= 4);
			u0 = s_nXyUp;
			s_Xy.touching = 0; s_Xy.active = 0; s_Xy.gen = 0x5151;
			for (int i = 0; i < 20; i++) { sidecar_xy_set (&s_Xy); lv_tick_inc (50); lv_timer_handler (); }
			EXPECT (s_nXyUp == u0);
		}
		// review L2: HOLD tapped, its command lost (the engine's view unchanged): the key shows the engine's again
		{
			xy_fake (4, 0, 0); s_Xy.gen = 0x5152; sidecar_xy_set (&s_Xy);
			sidecar_xy_hold_key ();
			EXPECT_CMD ("xy=hold:1");
			EXPECT (sidecar_xy_hold_shown ());
			s_Xy.x += 100; s_Xy.gen = 0x5153; sidecar_xy_set (&s_Xy);	// re-review N2: a position, not yet the HOLD
			EXPECT (sidecar_xy_hold_shown ());
			for (int i = 0; i < 20; i++) { sidecar_xy_set (&s_Xy); lv_tick_inc (50); lv_timer_handler (); }
			EXPECT (!sidecar_xy_hold_shown ());
		}
		xy_fake (4, 1, 1); sidecar_xy_set (&s_Xy); run (2); shot ("xy-hold");
		sidecar_xy_setup (1);
		EXPECT (sidecar_xy_setup_shown ());
		run (2); shot ("xy-setup");
		EXPECT (strstr (sidecar_xy_text (2 + 3), "V8:WID"));	// 2026-11: the summary names a param as its machine does
		sidecar_xy_pick (0, 2);
		EXPECT (sidecar_xy_picking () == 2);
		EXPECT (!strcmp (sidecar_xy_text (0), "X3: I8:BMIX") && !strcmp (sidecar_xy_text (1), "B4 MIX"));
		run (2); shot ("xy-pick");
		sidecar_xy_pick (0, 3);	// a voice's param: its number and its machine's name, "P16 WID"
		EXPECT (!strcmp (sidecar_xy_text (0), "X4: RS:WID") && !strcmp (sidecar_xy_text (1), "P16 WID"));
		run (2); shot ("xy-pick-voice");
		sidecar_xy_pick (1, 1);
		run (2); shot ("xy-pick-perf");
		sidecar_xy_pick (-1, 0);
		EXPECT (sidecar_xy_picking () == -1 && sidecar_xy_setup_shown ());
		sidecar_xy_setup (0);
		xy_fake (2, 0, 0); s_Xy.valid = 0; s_Xy.gen = 7; sidecar_xy_set (&s_Xy); run (2); shot ("xy-waiting");
		sidecar_ui_show (pg);
		EXPECT (!sidecar_xy_shown ());
	}
	// 0.5.1 HOME: Overview (the XY tile and the send FX tiles open their pages) and Perform (the pad, HOLD, SETUP, the quick keys)
	{
		int pg = sidecar_ui_page (), hx, hy, px, py, pw, ph;
		sidecar_ui_show (0); run (2);
		EXPECT (sidecar_xy_wanted () && !sidecar_xy_shown ());		// (the view is built for the XY tile)
		xy_fake (4, 1, 1); sidecar_xy_set (&s_Xy); s_Last[0] = 0; sidecar_ui_status (&s_St); run (2); shot ("home-overview");
		alert_bars (12), alert_bars (13);
		s_St.vmuted[6] = 1, s_St.underruns = 3; sidecar_ui_status (&s_St); run (2); shot ("home-overview-trouble");
		s_St.vmuted[6] = 0, s_St.underruns = 0; sidecar_ui_status (&s_St); run (2);
		EXPECT (sidecar_ui_home_hit (9, &hx, &hy)); tap (hx, hy);	// the XY tile: Perform
		EXPECT (sidecar_ui_page () == 24 && sidecar_xy_wanted () && !sidecar_xy_shown ());
		sidecar_ui_show (0); run (2);
		EXPECT (sidecar_ui_home_hit (10, &hx, &hy)); tap (hx, hy);	// a send FX tile: FX > Sends
		EXPECT (sidecar_ui_page () == 2);
		sidecar_ui_show (0); run (2);
		tap_raw (SUBX (4, 3), SUBY);					// the Perform pill
		EXPECT (sidecar_ui_page () == 24 && sidecar_xy_wanted ());
		xy_fake (4, 0, 0); sidecar_xy_set (&s_Xy); run (2); shot ("home-perform");
		EXPECT (sidecar_xy_perform_hit (0, &px, &py, &pw, &ph) && pw > 100 && ph > 100);
		int n0 = s_nXyPos;
		drag (px + pw / 8, py + ph * 7 / 8, px + pw * 3 / 4, py + ph / 4, 10, "home-perform-drag");	// live, as the XY page
		EXPECT (s_nXyPos - n0 >= 3);
		run (2);
		EXPECT (sidecar_xy_perform_hit (1, &px, &py, &pw, &ph)); tap (px + pw / 2, py + ph / 2);
		EXPECT_CMD ("xy=hold:1");
		xy_fake (4, 1, 1); sidecar_xy_set (&s_Xy); run (2); shot ("home-perform-hold");
		EXPECT (sidecar_ui_home_hit (16, &hx, &hy)); tap (hx, hy);
		EXPECT_CMD ("killfx");
		s_St.rec = 2; sidecar_ui_status (&s_St); run (2);
		EXPECT (sidecar_ui_home_hit (15, &hx, &hy)); tap (hx, hy);		// a take running: REC stops it
		EXPECT_CMD ("rec=stop");
		s_St.rec = 0; sidecar_ui_status (&s_St); run (2);
		tap (hx, hy); EXPECT (sidecar_ui_page () == 11);			// idle: the REC page (its warnings)
		sidecar_ui_show (24); run (2);
		EXPECT (sidecar_xy_perform_hit (2, &px, &py, &pw, &ph)); tap (px + pw / 2, py + ph / 2);	// SETUP: the XY page's setup
		EXPECT (sidecar_ui_page () == 22 && sidecar_xy_setup_shown ());
		sidecar_xy_setup (0);
		sidecar_ui_show (4); run (2);
		EXPECT (!sidecar_xy_wanted ());
		sidecar_ui_show (pg);
	}
	EXPECT (sidecar_insfx_voice () == 2);
	{	// touch controls: voice 3's dials (a base that is not the played value), sends and OUT (on POST), the console's lines
		ins_fake (13); sidecar_insfx_set (&s_Iv); run (2);
		EDIT_CHECK (sidecar_insfx_obj (0), 1, 83, 50, 127, 80, "ins=V3:A:p1:%d", push_ins);	// slot A P1: CUT
		EDIT_CHECK (sidecar_insfx_obj (5), 1, 43, 55, 127, 40, "ins=V3:B:p2:%d", push_ins);	// slot B P2: FDBK
		EDIT_CHECK (sidecar_insfx_obj (10), 0, 90, 0, 127, 90, "ins=V3:send:1:%d", push_ins);	// the send to bus 1
		EDIT_CHECK (sidecar_insfx_obj (12), 0, 64, 0, 127, 64, "ins=V3:send:3:%d", push_ins);	// the send to bus 3
		EDIT_CHECK (sidecar_insfx_obj (20), 0, 80, 100, 127, 90, "out=V3:lev:%d", push_ins);	// OUT LEV
		EDIT_CHECK (sidecar_insfx_obj (21), 0, 40, 64, 127, 44, "out=V3:pan:%d", push_ins);
		EDIT_CHECK (sidecar_insfx_obj (22), 0, 100, 64, 127, 100, "out=V3:wid:%d", push_ins);
	}
	{	// the followed voice changes under a finger (the Rytm selects another track): the drag and the held value stop, no more lines
		lv_obj_t *k = (lv_obj_t *) sidecar_insfx_obj (0), *sb = (lv_obj_t *) sidecar_insfx_obj (10);
		lv_area_t c;
		lv_obj_get_coords (k, &c);
		const int hx = (c.x1 + c.x2) / 2, hy = (c.y1 + c.y2) / 2;
		run (14);
		int c0 = s_nCmds;
		drag_hold_raw (hx, hy, hx, hy - TH->touch, 6);
		EXPECT (s_nCmds > c0 && th_meter_editing (k));
		c0 = s_nCmds;
		ins_fake (++s_nPushGen); s_Iv.voice = 5, s_Iv.track = 9;
		sidecar_insfx_set (&s_Iv); run (2);
		EXPECT (sidecar_insfx_voice () == 5 && !th_meter_editing (k));
		s_Px = sx (hx), s_Py = sy (hy - 2 * TH->touch); run (3);
		lift ();
		EXPECT (s_nCmds == c0);
		ins_fake (++s_nPushGen);					// back to voice 3 (the follow cancels again)
		sidecar_insfx_set (&s_Iv); run (2);
		EXPECT (sidecar_insfx_voice () == 2);
		// and a held value after a lift (300 ms) does not stay on the knob when the voice changes
		run (14);
		drag_hold_raw (hx, hy, hx, hy - TH->touch, 6), lift ();
		EXPECT (th_meter_editing (k));
		ins_fake (++s_nPushGen); s_Iv.voice = 5, s_Iv.track = 9;
		sidecar_insfx_set (&s_Iv); run (2);
		EXPECT (sidecar_insfx_voice () == 5 && !th_meter_editing (k) && !th_meter_editing (sb));
		ins_fake (++s_nPushGen);					// back to voice 3
		sidecar_insfx_set (&s_Iv); run (2);
		EXPECT (sidecar_insfx_voice () == 2);
		run (14);
	}
	{ int hx, hy; EXPECT (sidecar_insfx_hit (1, &hx, &hy)); tap_raw (hx, hy); }			// slot A's TYPE: FILTER -> the next
	EXPECT_CMD ("ins=V3:A:type:CRUSH");
	{ int hx, hy; EXPECT (sidecar_insfx_hit (2, &hx, &hy)); tap_raw (hx, hy); }	// the OUT key (voice 3 on POST): the next route, TRK
	EXPECT_CMD ("out=V3:trk");
	{ int hx, hy; EXPECT (sidecar_insfx_hit (0, &hx, &hy)); tap_raw (hx, hy); }		// V1: another voice (nothing set)
	EXPECT (sidecar_insfx_voice () == 0);
	run (2); shot ("insfx-empty");
	EDIT_NONE (sidecar_insfx_obj (0));		// voice 1: no slot TYPE set, route TRK: no dial to turn, no OUT stage to set
	EDIT_NONE (sidecar_insfx_obj (4));
	EDIT_NONE (sidecar_insfx_obj (20));
	EDIT_NONE (sidecar_insfx_obj (22));
	// the SONG page: a take replaying (the playhead), recording, the replay's confirmation, the how-to, zoomed in
	sidecar_ui_show (10);
	EXPECT (sidecar_song_shown () && sidecar_ui_page () == 10);
	song_fake (SSG_OFF, 0); sidecar_song_set (&s_Sv); run (2); shot ("song-timeline");
	song_fake (SSG_REPLAYING, 19 * SSG_BAR + 5 * SSG_PPQ); sidecar_song_set (&s_Sv); run (2); shot ("song-replaying");
	{
		int ky = o ? LU (224) + DY : LU (52) + LU (366) + DY + LU (27);
#define SONG_KEY(k) (o ? (k) * (LU (44) + DX / 8) + LU (21) + ((k) > 3 ? 2 : 0) : LU (12) + (k) * (LU (97) + DX / 8) + LU (46))
		song_fake (SSG_OFF, 0); sidecar_song_set (&s_Sv); run (2);
		tap (SONG_KEY (0), ky); EXPECT_CMD ("song=arm");
		tap (SONG_KEY (1), ky); EXPECT_CMD ("song=replay");
		tap (SONG_KEY (3), ky); EXPECT_CMD ("song=next");
		// UX review T14: after a replay UNDO REPLAY in < > 's place (one key over both), while the engine offers it
		song_fake (SSG_OFF, 0); strcpy (s_Sv.msg, "replay done: 1834 sent, 0 not replayable"); s_Sv.undo = 1;
		sidecar_song_set (&s_Sv); run (2); shot ("song-undo");
		tap ((SONG_KEY (2) + SONG_KEY (3)) / 2, ky); EXPECT_CMD ("song=undo");
		song_fake (SSG_OFF, 0); sidecar_song_set (&s_Sv); run (2);
		tap (SONG_KEY (2), ky); EXPECT_CMD ("song=prev");	// (< > back once it is no longer offered)
		song_fake (SSG_CONFIRM, 0); sidecar_song_set (&s_Sv); run (2); shot ("song-confirm");
		if (o) tap (DX / 2 + LU (9) + LU (180) + LU (73), DY / 2 + LU (20) + LU (178) + LU (16)); else tap (DX / 2 + LU (120) + LU (300) + LU (118), DY / 2 + LU (52) + LU (40) + LU (270) + LU (25));
		EXPECT_CMD ("song=confirm");
		if (o) tap (DX / 2 + LU (9) + LU (8) + LU (73), DY / 2 + LU (20) + LU (178) + LU (16)); else tap (DX / 2 + LU (120) + LU (24) + LU (118), DY / 2 + LU (52) + LU (40) + LU (270) + LU (25));
		EXPECT_CMD ("song=cancel");
		song_fake (SSG_OFF, 0); strcpy (s_Sv.config, "ENCODER DEST INT, OUTPUT CH AUTO"); s_Sv.cfg_wrong = 3; s_Sv.gen++;
		sidecar_song_set (&s_Sv); run (2);
		tap_raw (SUBX (2, sidecar_song_explain_shown () ? 0 : 1), SUBY);		// the Timeline / Setup pills
		run (2); shot ("song-howto");
		tap_raw (SUBX (2, sidecar_song_explain_shown () ? 0 : 1), SUBY);		// the Timeline / Setup pills
		song_fake (SSG_REC, 13 * SSG_BAR + 2 * SSG_PPQ); s_Sv.length = 13 * SSG_BAR; sidecar_song_set (&s_Sv); run (2);
		tap (SONG_KEY (5), ky); tap (SONG_KEY (5), ky); tap (SONG_KEY (5), ky);	// + + +: 16 bars across
		EXPECT (sidecar_song_zoom () == 16);
		run (2); shot ("song-recording-zoom");
		// a drag scrolls (FOLLOW off)
		song_fake (SSG_OFF, 0); sidecar_song_set (&s_Sv); run (2);
		if (o) drag (LU (270), LU (90), LU (90), LU (90), 6, "song-scrolled"); else drag (LU (600), LU (52) + LU (200), LU (200), LU (52) + LU (200), 6, "song-scrolled");
		tap (SONG_KEY (4), ky); tap (SONG_KEY (4), ky); tap (SONG_KEY (4), ky); tap (SONG_KEY (4), ky);
		EXPECT (sidecar_song_zoom () == 0);
#undef SONG_KEY
	}
	// the REC page (UP + T7): idle, armed, recording; a channel tap, ALL, the takes and the delete's confirmation
	sidecar_ui_show (11);
	EXPECT (sidecar_rec_shown () && sidecar_ui_page () == 11 && !sidecar_song_shown ());
	{
		const int Wd = o ? LU (352) + DX : LU (800) + DX;
		const int ky = o ? LU (15) + LU (23) : LU (52) + LU (6) + LU (31);
		const int kx = LU (16) + LU (800) + DX - LU (210);		// (the display window ends 210 px before the page's edge; REC and ARM right of it)
		const int rec_x = o ? LU (31) : kx + LU (50), arm_x = o ? LU (64) + LU (23) : kx + LU (106) + LU (40);
		const int tak_x = o ? LU (301) + DX + LU (25) : LU (716) + DX + (LU (800) + DX - LU (8) - (LU (716) + DX)) / 2;
		const int cw = o ? (Wd - LU (4)) / 3 : (Wd - LU (32)) / 3, ph = o ? (LU (168) + DY) / 8 : (LU (336) + DY) / 8;
#define REC_CX(c) (o ? (c) * (cw + 2) + cw / 2 : LU (8) + (c) * (cw + LU (8)) + cw / 2)
#define REC_CY(r) ((o ? LU (72) : LU (52) + LU (88)) + (r) * ph + ph / 2)
		rec_play (SRV_IDLE, 83, 12); shot ("rec-idle");
		tap (rec_x, ky); EXPECT_CMD ("rec=start");
		tap (arm_x, ky); EXPECT_CMD ("rec=arm");
		tap (REC_CX (0), REC_CY (0)); EXPECT_CMD ("rec=ch:-3");		// BD's stem: armed -> off
		tap (REC_CX (1), REC_CY (1)); EXPECT_CMD ("rec=ch:+15");	// the Sidecar's SD voice (voice 3)
		tap (REC_CX (2), REC_CY (7)); EXPECT_CMD ("rec=ch:all");
		rec_play (SRV_ARMED, 83, 6); shot ("rec-armed");
		tap (arm_x, ky); EXPECT_CMD ("rec=stop");			// DISARM
		rec_play (SRV_REC, 3723, 14); shot ("rec-recording");
		snprintf (s_Last, sizeof s_Last, "none");
		tap (REC_CX (1), REC_CY (2)); EXPECT_CMD ("none");		// no selection while a take records
		tap (arm_x, ky); EXPECT_CMD ("none");
		tap (rec_x, ky); EXPECT_CMD ("rec=stop");
		rec_play (SRV_IDLE, 3723, 2);
		tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
		EXPECT (sidecar_rec_takes_shown ());
		run (2); shot ("rec-takes");
		{	// playback: a tap on a take plays / pauses it, STOP resets; the playing row's glyph, frame and bar
			const int rx = o ? LU (100) : LU (300), stop_x = arm_x;
			const int rh0 = o ? (ph < LU (18) ? LU (18) : ph) : (LU (312) + DY) / ((LU (312) + DY) / LU (40) < 8 ? (LU (312) + DY) / LU (40) : 8);
#define REC_RY(i) (o ? LU (62) + LU (10) + (i) * rh0 + rh0 / 2 : LU (52) + LU (72) + LU (34) + (i) * rh0 + rh0 / 2)
			tap (rx, REC_RY (0)); EXPECT_CMD ("play=toggle:take_004");
			tap (rx, REC_RY (2)); EXPECT_CMD ("play=toggle:take_002");
			s_PlSt = SRV_PL_PLAYING, s_PlTake = "take_002", s_PlLenMs = 3725000;
			for (int f = 0; f < 4; f++) { s_PlMs = 1260000 + f * 100; rec_fake (SRV_IDLE, 3723, 2); sidecar_rec_set (&s_Rv); run (8); }
			shot ("rec-playing");
			s_PlSt = SRV_PL_PAUSED, s_PlMs = 1263000;
			rec_fake (SRV_IDLE, 3723, 2); sidecar_rec_set (&s_Rv); run (2); shot ("rec-paused");
			s_PlSt = SRV_PL_PLAYING, s_PlTake = "take_004", s_PlLenMs = 83000, s_PlMs = 20000;
			rec_fake (SRV_IDLE, 3723, 2); sidecar_rec_set (&s_Rv); run (2);
			tap (stop_x, ky); EXPECT_CMD ("play=stop");
			s_PlSt = 0;
			rec_fake (SRV_IDLE, 3723, 2); sidecar_rec_set (&s_Rv); run (2);
#undef REC_RY
		}
		const int dl_x = o ? Wd - LU (24) : Wd - LU (74), rh = o ? (ph < LU (18) ? LU (18) : ph) : (LU (312) + DY) / ((LU (312) + DY) / LU (40) < 8 ? (LU (312) + DY) / LU (40) : 8);
#define REC_DY(i) (o ? LU (62) + LU (10) + (i) * rh + rh / 2 : LU (52) + LU (72) + LU (34) + (i) * rh + rh / 2)
		snprintf (s_Last, sizeof s_Last, "none");
		tap (dl_x, REC_DY (1));
		run (2); shot ("rec-delete");
		EXPECT_CMD ("none");
		const int dx0 = o ? (Wd - LU (300)) / 2 : (Wd - LU (520)) / 2, dy0 = o ? LU (66) + DY / 2 : LU (52) + LU (100) + DY / 2;
		if (o) tap (dx0 + LU (8) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (24) + LU (112), dy0 + LU (146) + LU (28));	// CANCEL
		EXPECT_CMD ("none");
		tap (dl_x, REC_DY (1));
		if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));	// DELETE
		EXPECT_CMD ("rec=del:take_003");
		tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
		EXPECT (!sidecar_rec_takes_shown ());
		s_Rv.valid = 0; sidecar_rec_set (&s_Rv); run (2); shot ("rec-off");
		{	// the destination a USB drive: named with its space; a take on it plays and deletes by its id
			s_RecUsb = 1;
			rec_play (SRV_IDLE, 83, 2); shot ("rec-usb");
			s_RecUsb = 2; rec_play (SRV_IDLE, 83, 2); shot ("rec-usb-missing");	// the drive not mounted
			s_RecUsb = 3; rec_play (SRV_IDLE, 83, 2); shot ("rec-usb-nofree");	// its free space not counted
			s_RecUsb = 1;
			tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
			EXPECT (sidecar_rec_takes_shown ());
			run (2); shot ("rec-takes-usb");
			tap (o ? LU (100) : LU (300), REC_DY (1)); EXPECT_CMD ("play=toggle:USB:take_003");
			tap (dl_x, REC_DY (1));
			run (2); shot ("rec-delete-usb");
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));	// DELETE
			EXPECT_CMD ("rec=del:USB:take_003");
			tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
			EXPECT (!sidecar_rec_takes_shown ());
			s_RecUsb = 0;
		}
		{	// 5d: many takes, newest first: a vertical drag scrolls them a row at a time (the header says which show)
			char want[96];
			const int rx = o ? LU (100) : LU (300), nr = o ? ((TH->h - LU (25) - LU (62) - 10) / rh < 8 ? (TH->h - LU (25) - LU (62) - 10) / rh : 8) : (LU (312) + DY) / LU (40) < 8 ? (LU (312) + DY) / LU (40) : 8;
			s_RecMany = 1;
			rec_play (SRV_IDLE, 83, 2);
			tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
			EXPECT (sidecar_rec_takes_shown ());
			run (2); shot ("rec-takes-many");
			EXPECT (text_shown (lv_screen_active (), o ? "TAKE 1-" : "Take   1-"));
			snprintf (s_Last, sizeof s_Last, "none");
			drag (rx, REC_DY (nr > 5 ? 5 : nr - 1), rx, REC_DY (nr > 5 ? 1 : nr - 5), 8, 0);		// four rows up: the older takes (800x480: five rows fit)
			EXPECT_CMD ("none");					// (a drag plays nothing)
			run (2); shot ("rec-takes-scrolled");
			EXPECT (text_shown (lv_screen_active (), o ? "TAKE 5-" : "Take   5-"));
			tap (rx, REC_DY (0));
			snprintf (want, sizeof want, "play=toggle:%s", rec_many (4)); EXPECT_CMD (want);
			drag (rx, REC_DY (0), rx, REC_DY (nr - 1), 8, 0);	// down past the top: the newest again
			tap (rx, REC_DY (0));
			snprintf (want, sizeof want, "play=toggle:%s", rec_many (0)); EXPECT_CMD (want);
			for (int k = 0; k < 6; k++) drag (rx, REC_DY (nr - 1), rx, REC_DY (0), 8, 0);	// to the end: the oldest last
			run (2); shot ("rec-takes-end");
			tap (rx, REC_DY (nr - 1));
			snprintf (want, sizeof want, "play=toggle:%s", rec_many (22)); EXPECT_CMD (want);
			tap (dl_x, REC_DY (nr - 1));					// the delete of a scrolled row: that take
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));
			snprintf (want, sizeof want, "rec=del:%s", rec_many (22)); EXPECT_CMD (want);
			tap (dl_x, REC_DY (nr - 1));					// the list changes under an open dialog:
			s_Rv.takes_gen++, s_Rv.ntakes = 22;				// the take asked about gone - the dialog closes
			sidecar_rec_set (&s_Rv); run (2);
			snprintf (s_Last, sizeof s_Last, "none");
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));	// (a row there now)
			EXPECT (strncmp (s_Last, "rec=del:", 8) != 0);			// no delete
			rec_play (SRV_IDLE, 83, 2);						// (all 23 again: the top kept)
			tap (dl_x, REC_DY (0));						// a newer take inserted before it: still that one
			for (int i = 0; i < 22; i++) s_Rv.takes[22 - i] = s_Rv.takes[21 - i];
			snprintf (s_Rv.takes[0].name, SRV_NAME, "jam-night_2026-10-28_0900");
			s_Rv.takes_gen++;
			sidecar_rec_set (&s_Rv); run (2);
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));
			snprintf (want, sizeof want, "rec=del:%s", rec_many (22 - nr)); EXPECT_CMD (want);	// (the top: 22 takes less the rows)
			tap_raw (SUBX (2, sidecar_rec_takes_shown () ? 0 : 1), SUBY);		// the Record / Takes pills
			EXPECT (!sidecar_rec_takes_shown ());
			s_RecMany = 0;
		}
		{	// 5d: the warning before a take: the selection needs 82 % of the card's 5.0 MB/s - REC and ARM ask first
			s_RecWarn = 1;
			rec_play (SRV_IDLE, 83, 2);
			snprintf (s_Last, sizeof s_Last, "none");
			tap (rec_x, ky); EXPECT_CMD ("none");
			run (2); shot ("rec-warn");
			EXPECT (text_shown (lv_screen_active (), o ? "82 %" : "82%"));
			if (o) tap (dx0 + LU (8) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (24) + LU (112), dy0 + LU (146) + LU (28));	// CANCEL
			EXPECT_CMD ("none");
			tap (rec_x, ky);
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));	// RECORD
			EXPECT_CMD ("rec=start");
			snprintf (s_Last, sizeof s_Last, "none");
			tap (arm_x, ky); EXPECT_CMD ("none");
			run (2); shot ("rec-warn-arm");
			if (o) tap (dx0 + LU (156) + LU (68), dy0 + LU (70) + LU (16)); else tap (dx0 + LU (272) + LU (112), dy0 + LU (146) + LU (28));	// ARM
			EXPECT_CMD ("rec=arm");
			s_RecWarn = 0;
			rec_play (SRV_IDLE, 83, 2);
			tap (rec_x, ky); EXPECT_CMD ("rec=start");		// a selection that fits: at once
		}
		rec_play (SRV_IDLE, 83, 2);
#undef REC_CX
#undef REC_CY
#undef REC_DY
		// the home page's REC status opens it
		sidecar_ui_show (0); run (2);
		if (o) tap (OW_HIT_X, LU (6)); else { int hx, hy; EXPECT (sidecar_ui_home_hit (12, &hx, &hy)); tap (hx, hy); }	// (the status line's Recorder)
		EXPECT (sidecar_ui_page () == 11);
	}

	// ---- touch2: SCOPE (UP + T9): TRACK follows the Rytm's track (BD's kick, triggered), a meter's tap shows its
	// channel (MAIN), the timebase up to a sweep, GAIN, FREEZE, TRACK again
	sidecar_ui_show (12);
	EXPECT (sidecar_scope_shown () && sidecar_ui_page () == 12 && !sidecar_rec_shown ());
	sidecar_scope_debug (-1, 1, 0, 0);
	{
		const int pw = o ? LU (352) + DX : LU (800) + DX, y0 = o ? 0 : LU (52);
		const int ah = o ? LU (133) + DY * 2 / 3 : LU (196) + DY * 2 / 3, sy = y0 + (o ? LU (18) + ah : LU (254) + DY * 2 / 3), sh = o ? LU (63) + DY / 3 : LU (108) + DY / 3;
		const int sxx = o ? 0 : LU (8), gap = o ? 2 : LU (8), cw = ((o ? pw : pw - LU (16)) + gap) / 12 - gap, q = DX / 4;
		const int ky = o ? LU (218) + DY + LU (11) : LU (52) + LU (372) + DY + LU (26);
#define SC_COL(k) (sxx + (k) * (cw + gap) + cw / 2)
		for (int f = 0; f < 6; f++) { scope_fake (f); sidecar_scope_set (&s_Sc); run (1); }
		shot ("scope-track");
		tap (SC_COL (0), sy + sh / 2);
		EXPECT (sidecar_scope_source () == 0);
		for (int f = 6; f < 10; f++) { scope_fake (f); sidecar_scope_set (&s_Sc); run (1); }
		shot ("scope-main");
		tap (o ? LU (100) + q : LU (206) + q, ky); EXPECT (sidecar_scope_timebase () == 0);	// TIME <
		for (int i = 0; i < 4; i++) tap (o ? LU (222) + 2 * q : LU (502) + 2 * q, ky);		// TIME > x 4: 683 ms, a sweep
		EXPECT (sidecar_scope_timebase () == 4);
		tap (o ? (LU (246) + 2 * q + pw) / 2 : (LU (550) + 2 * q + pw - LU (8)) / 2, ky);		// GAIN: x1
		EXPECT (sidecar_scope_gain () == 1);
		tap (SC_COL (1), sy + sh / 2);							// BD
		EXPECT (sidecar_scope_source () == 1);
		for (int f = 10; f < 16; f++) { scope_fake (f); sidecar_scope_set (&s_Sc); run (1); }
		shot ("scope-sweep");
		tap (o ? (LU (76) + q) / 2 : LU (83) + q / 2, ky);					// FREEZE
		EXPECT (sidecar_scope_frozen ());
		for (int f = 16; f < 20; f++) { scope_fake (f); sidecar_scope_set (&s_Sc); run (1); }
		shot ("scope-frozen");
		tap (o ? (LU (76) + q) / 2 : LU (83) + q / 2, ky);					// FREEZE off
		EXPECT (!sidecar_scope_frozen ());
		tap (sxx + 11 * (cw + gap) + (pw - (o ? 0 : LU (8)) - (sxx + 11 * (cw + gap))) / 2, sy + sh / 2);	// TRACK
		EXPECT (sidecar_scope_source () == -1);
		sidecar_scope_debug (-1, 1, 0, 0);
		s_Sc.valid = 0; s_Sc.gen++; sidecar_scope_set (&s_Sc); run (2); shot ("scope-nolink");
		scope_fake (21); sidecar_scope_set (&s_Sc); run (2);
#undef SC_COL
	}
	// ---- CHOP (page 17): the waveform with its slices, the pads (bank A), the list; a pad auditions, LAZY arms, a
	// marker's drag moves it, the list's + edits the cursor's value; LAZY's view, the zoom, a RESAMPLE's offer, no voice
	sidecar_ui_show (17);
	EXPECT (sidecar_chop_shown () && sidecar_ui_page () == 17 && sidecar_ui_nav_lit () >= 0);
	sidecar_chop_debug (3, 0, 0, 0);
	chop_fake (0); sidecar_chop_set (&s_Ch); run (2); shot ("chop");
	{
		int x, y;
		// a marker under the finger holds the page (the display must not leave CHOP before the move is sent at the release)
		EXPECT (!sidecar_chop_dragging ());
		EXPECT (sidecar_chop_hit (4, 22, &x, &y));				// the marker at frame 42300 of 192000, 22.03 %)
		s_Px = sx (x); s_Py = sy (y); s_Down = 1; run (3);
		EXPECT (sidecar_chop_dragging ());
		s_Down = 0; run (3);
		EXPECT (!sidecar_chop_dragging ());
		EXPECT (!strncmp (sidecar_chop_last_command (), "chop=start:0:4:", 15));	// (sent at the release)
		EXPECT (sidecar_chop_hit (0, 5, &x, &y)); tap_raw (x, y);
		EXPECT (!strcmp (s_Last, "chop=play:0:5") && sidecar_chop_sel () == 4);
		EXPECT (sidecar_chop_hit (1, 0, &x, &y)); tap_raw (x, y);
		EXPECT (!strcmp (s_Last, "chop=lazy:0:arm"));
		EXPECT (sidecar_chop_hit (2, 5, &x, &y)); tap_raw (x, y);			// + on the cursor (PTCH of slice 5)
		EXPECT (!strcmp (s_Last, "chop=set:0:5:PTCH:1"));
		EXPECT (sidecar_chop_hit (2, 1, &x, &y)); tap_raw (x, y);			// DOWN: slice 6
		EXPECT (sidecar_chop_sel () == 5);
		{	// the list's first row: slice 1, or (fewer rows than six show: 800x480) the first one scrolled to
			int nr = 0;
			while (sidecar_chop_hit (3, nr, &x, &y)) nr++;
			EXPECT (sidecar_chop_hit (3, 0, &x, &y)); tap_raw (x, y);
			EXPECT (sidecar_chop_sel () == (nr > 5 ? 0 : 5 - nr + 1));
		}
		EXPECT (sidecar_chop_hit (1, 3, &x, &y)); tap_raw (x, y);			// GRID: the next count (32)
		EXPECT (!strcmp (s_Last, "chop=grid:0:32"));
		sidecar_chop_debug (5, 3, 0, 9);
		chop_fake (0); sidecar_chop_set (&s_Ch); run (2); shot ("chop-zoom");
		chop_fake (1); sidecar_chop_set (&s_Ch); run (2);
		EXPECT (sidecar_chop_hit (0, 9, &x, &y)); tap_raw (x, y);			// LAZY: a pad taps
		EXPECT (!strcmp (s_Last, "chop=lazy:0:tap"));
		sidecar_chop_debug (0, 0, 0, 0);
		chop_fake (1); sidecar_chop_set (&s_Ch); run (2); shot ("chop-lazy");
		chop_fake (2); sidecar_chop_set (&s_Ch); run (2); shot ("chop-offer");
		EXPECT (sidecar_chop_hit (1, 7, &x, &y)); tap_raw (x, y);			// YES: replace
		EXPECT (!strcmp (s_Last, "chop=replace:yes"));
		sidecar_chop_debug (2, 1, 1, 4);
		chop_fake (0); sidecar_chop_set (&s_Ch); run (2); shot ("chop-bank-b");
		chop_fake (-1); sidecar_chop_set (&s_Ch); run (2); shot ("chop-none");
	}
	// ---- SAMPLE EDIT (page 21): the edit's waveform with its region; a marker's drag sends the start at the release, a
	// function key applies, SAVE asks SAVE + USE / SAVE (SAVE + USE only from a track), CLOSE asks before it drops an edit
	sidecar_ui_show (21);
	EXPECT (sidecar_sedit_shown () && sidecar_ui_page () == 21 && !sidecar_chop_shown ());
	sedit_fake (0); sidecar_sedit_set (&s_Se); run (2); shot ("sedit");
	{
		int x, y, x1, y1;
		EXPECT (sidecar_sedit_hit (0, 0, &x, &y));				// the start marker
		EXPECT (!sidecar_sedit_dragging ());
		s_Px = sx (x); s_Py = sy (y); s_Down = 1; run (3);
		for (int i = 1; i <= 5; i++) { s_Px = sx (x + 20 * i); run (1); }
		EXPECT (sidecar_sedit_dragging ());
		sedit_fake (0); sidecar_sedit_set (&s_Se); run (2);			// a new view while held: the marker stays under the finger
		EXPECT (sidecar_sedit_hit (0, 0, &x1, &y1) && x1 > x + 80);
		shot ("sedit-drag");
		s_Down = 0; run (3);
		EXPECT (!sidecar_sedit_dragging ());
		EXPECT (!strncmp (s_Last, "sedit=start:", 12) && atoi (s_Last + 12) > 2400);	// (sent at the release)
		{	// the engine refused it (its views keep the old start, the same gen): held a moment, then the engine's again
			int xs, xe;
			EXPECT (sidecar_sedit_hit (0, 0, &xs, &y1) && xs > x + 80);
			sidecar_sedit_set (&s_Se); run (1);
			EXPECT (sidecar_sedit_hit (0, 0, &x1, &y1) && x1 == xs);
			for (int i = 0; i < 6; i++) sidecar_sedit_set (&s_Se);
			run (1);
			EXPECT (sidecar_sedit_hit (0, 0, &xe, &y1) && xe == x);
		}
		EXPECT (sidecar_sedit_hit (0, 1, &x, &y));				// the end marker, to the left
		drag_raw (x, y, x - 60, y, 4, 0);
		EXPECT (!strncmp (s_Last, "sedit=end:", 10) && atoi (s_Last + 10) < 48000);
		s_Se.b = (unsigned) atoi (s_Last + 10); s_Se.gen++;			// taken: the engine's view has it
		sidecar_sedit_set (&s_Se); run (1);
		EXPECT (sidecar_sedit_hit (0, 1, &x1, &y1) && x1 < x - 40);
		sedit_fake (0); sidecar_sedit_set (&s_Se); run (1);			// (the region back to the fake's)
		{	// a press lost (LVGL's wait-for-release: the press taken away mid-drag): nothing sent, the engine's marker again
			int xe;
			EXPECT (sidecar_sedit_hit (0, 1, &x, &y));
			snprintf (s_Last, sizeof s_Last, "-");
			s_Px = sx (x); s_Py = sy (y); s_Down = 1; run (3);
			for (int i = 1; i <= 4; i++) { s_Px = sx (x - 15 * i); run (1); }
			EXPECT (sidecar_sedit_dragging ());
			lv_indev_wait_release (lv_indev_get_next (NULL)); run (3);
			s_Down = 0; run (3);
			EXPECT (!sidecar_sedit_dragging ());
			EXPECT_CMD ("-");
			EXPECT (sidecar_sedit_hit (0, 1, &xe, &y1) && xe == x);
		}
		EXPECT (sidecar_sedit_hit (1, SSE_FN_CROP, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=apply:CROP");
		EXPECT (sidecar_sedit_hit (1, SSE_FN_GAIN_DN, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=apply:GAIN-");
		EXPECT (sidecar_sedit_hit (2, SSE_K_UNDO, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=undo");
		EXPECT (sidecar_sedit_hit (2, SSE_K_PLAY, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=preview");
		EXPECT (sidecar_sedit_hit (2, SSE_K_ZOOM_IN, &x, &y)); tap_raw (x, y);	// half the window, around its middle
		EXPECT_CMD ("sedit=window:14400:43200");
		sedit_fake (0); s_Se.wa = 14400, s_Se.wb = 43200; sidecar_sedit_set (&s_Se); run (2);	// (the engine's window followed)
		EXPECT (sidecar_sedit_hit (2, SSE_K_ZOOM_OUT, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=window:0:57600");
		EXPECT (sidecar_sedit_hit (4, 50, &x, &y));				// a drag away from the markers pans
		drag_raw (x, y, x - 100, y, 4, 0);
		EXPECT (!strncmp (s_Last, "sedit=window:", 13) && atoi (s_Last + 13) > 14400);
		shot ("sedit-zoom");
		sedit_fake (0); sidecar_sedit_set (&s_Se); run (2);
		EXPECT (!sidecar_sedit_hit (3, SSE_D_USE, &x, &y));			// no question yet
		EXPECT (sidecar_sedit_hit (2, SSE_K_SAVE, &x, &y)); tap_raw (x, y); run (1);
		shot ("sedit-save");
		EXPECT (sidecar_sedit_hit (3, SSE_D_USE, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=save:use");
		EXPECT (!sidecar_sedit_hit (3, SSE_D_USE, &x, &y));			// (answered: gone)
		EXPECT (sidecar_sedit_hit (2, SSE_K_SAVE, &x, &y)); tap_raw (x, y);
		EXPECT (sidecar_sedit_hit (3, SSE_D_OK, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=save");
		EXPECT (sidecar_sedit_hit (2, SSE_K_CLOSE, &x, &y)); tap_raw (x, y);	// dirty: asked first
		EXPECT (strcmp (s_Last, "sedit=close"));
		shot ("sedit-discard");
		EXPECT (sidecar_sedit_hit (3, SSE_D_CANCEL, &x, &y)); tap_raw (x, y);
		EXPECT (strcmp (s_Last, "sedit=close") && !sidecar_sedit_hit (3, SSE_D_CANCEL, &x, &y));
		EXPECT (sidecar_sedit_hit (2, SSE_K_CLOSE, &x, &y)); tap_raw (x, y);
		EXPECT (sidecar_sedit_hit (3, SSE_D_OK, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=close");
		sedit_fake (1); sidecar_sedit_set (&s_Se); run (2); shot ("sedit-pool");
		EXPECT (sidecar_sedit_hit (2, SSE_K_PLAY, &x, &y)); tap_raw (x, y);	// sedit-050: PLAY from the pool too (a DSP SAMPLE track)
		EXPECT_CMD ("sedit=preview");
		s_Se.wa = 48000, s_Se.wb = 96000, s_Se.zoom = 2, s_Se.unity = 23170, s_Se.gen++;	// zoomed in, over 0 dBFS (+3 dB)
		sidecar_sedit_set (&s_Se); run (2); shot ("sedit-pool-zoom-over");
		sedit_fake (1); sidecar_sedit_set (&s_Se); run (2);
		EXPECT (sidecar_sedit_hit (2, SSE_K_SAVE, &x, &y)); tap_raw (x, y); run (1);
		EXPECT (!sidecar_sedit_hit (3, SSE_D_USE, &x, &y));			// from the pool: no SAVE + USE
		shot ("sedit-pool-save");
		EXPECT (sidecar_sedit_hit (3, SSE_D_CANCEL, &x, &y)); tap_raw (x, y);
		EXPECT (sidecar_sedit_hit (2, SSE_K_CLOSE, &x, &y)); tap_raw (x, y);	// nothing to drop: closed at once
		EXPECT_CMD ("sedit=close");
		// MAKE WT: the key makes the table (a console command), FRAMES takes the next choice (16 32 64 128 256 and round);
		// while it is made the page says so; from a WAVETABLE voice the edit is a table's file: no PLAY, MAKE WT first
		sedit_fake (1); sidecar_sedit_set (&s_Se); run (2);
		EXPECT (sidecar_sedit_hit (2, SSE_K_WT, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=makewt");
		EXPECT (sidecar_sedit_hit (2, SSE_K_FR, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=wtn:128");
		s_Se.wt_frames = 256; s_Se.gen++; sidecar_sedit_set (&s_Se); run (1);
		tap_raw (x, y);
		EXPECT_CMD ("sedit=wtn:16");
		sedit_fake (3); sidecar_sedit_set (&s_Se); run (2); shot ("sedit-make-wt");
		snprintf (s_Last, sizeof s_Last, "-");
		EXPECT (sidecar_sedit_hit (2, SSE_K_WT, &x, &y)); tap_raw (x, y);	// busy: the key waits
		EXPECT_CMD ("-");
		sedit_fake (4); sidecar_sedit_set (&s_Se); run (2); shot ("sedit-table-file");
		EXPECT (sidecar_sedit_hit (2, SSE_K_PLAY, &x, &y));
		tap_raw (x, y);
		EXPECT_CMD ("-");					// (a file has no PLAY)
		EXPECT (sidecar_sedit_hit (2, SSE_K_WT, &x, &y)); tap_raw (x, y);
		EXPECT_CMD ("sedit=makewt");
		sedit_fake (2); sidecar_sedit_set (&s_Se); run (2); shot ("sedit-busy");
		snprintf (s_Last, sizeof s_Last, "-");
		EXPECT (sidecar_sedit_hit (1, SSE_FN_CROP, &x, &y)); tap_raw (x, y);	// busy: the functions wait
		EXPECT_CMD ("-");
		// an edit open: the VOICE key leads back to it from another page; closed: VOICE as ever
		sidecar_ui_show (8); run (2);
		EXPECT (!sidecar_sedit_shown ());
		if (o) tap_raw (NAV_X, NAV_Y (3)); else tap_raw (CNAV_X (3), CNAV_Y (3));
		EXPECT (sidecar_sedit_shown () && sidecar_ui_page () == 21 && sidecar_ui_nav_lit () == 3);
		s_Se.valid = 0, s_Se.st = SSE_ST_CLOSED, s_Se.gen++; sidecar_sedit_set (&s_Se); run (2); shot ("sedit-closed");
		sidecar_ui_show (0); run (2);
		if (o) tap_raw (NAV_X, NAV_Y (3)); else tap_raw (CNAV_X (3), CNAV_Y (3));
		EXPECT (!sidecar_sedit_shown () && sidecar_ui_page () == 7);	// (Source is the remembered pill: the machine view)
		sidecar_ui_show (99);							// out of range: HOME (not the last page)
		EXPECT (sidecar_ui_page () == 0 && !sidecar_sedit_shown ());
		sidecar_ui_show (-1);
		EXPECT (sidecar_ui_page () == 0);
	}
	// ---- touch2: SIDECAR STATUS (UP + T10); the home page's engine numbers open it
	sidecar_ui_show (13);
	EXPECT (sidecar_status_shown () && sidecar_ui_page () == 13 && !sidecar_scope_shown ());
	status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status");
	status_fake (1); sidecar_status_set (&s_Stv); run (2); shot ("status-alerts");
	// the temperature (pi/engine/thermal.h; the figures from before 80 / 75, as the page shows what it is sent): 76 C
	// warned; 74 C still warned (hysteresis); 69 C cleared. TEMP in the alert
	// colour while the warning is on
	s_FakeTemp = 76, s_FakeWarn = 1; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-hot");
	EXPECT (text_shown (lv_screen_active (), o ? "76C HOT" : "76 \xC2\xB0" "C  hot"));
	s_FakeTemp = 74, s_FakeWarn = 1; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-hot-74");
	EXPECT (text_shown (lv_screen_active (), o ? "74C HOT" : "74 \xC2\xB0" "C  hot"));
	s_FakeTemp = 69, s_FakeWarn = 0; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-cool-69");
	EXPECT (!text_shown (lv_screen_active (), "HOT") && !text_shown (lv_screen_active (), "hot"));
	s_FakeMHz = 1000, s_FakeTemp = 66; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-1000");	// the clock option: 1000 MHz
	EXPECT (text_shown (lv_screen_active (), o ? "1000 MHZ" : "1000 MHz"));
	// 0.3.0: Circle's throttle lowered the clock (socmaxtemp 78, 600 MHz): warned at 81 the title line says it in the
	// alert colour; at 79, under the warning, quietly (no HOT on TEMP; OLED: CLOCK LOWERED, the line's room)
	s_FakeMHz = 1500, s_FakeTemp = 81, s_FakeWarn = 1, s_FakeLow = 600; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-hot-lowered");
	EXPECT (text_shown (lv_screen_active (), o ? "81C HOT" : "81 \xC2\xB0" "C  hot") && text_shown (lv_screen_active (), o ? "CLOCK LOWERED" : "clock lowered (hot)"));
	s_FakeTemp = 79, s_FakeWarn = 0; status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status-lowered-79");
	EXPECT (!text_shown (lv_screen_active (), o ? "79C HOT" : "79 \xC2\xB0" "C  hot") && text_shown (lv_screen_active (), o ? "CLOCK LOWERED" : "600 MHz: clock lowered (hot)"));
	s_FakeLow = 0;
	s_FakeMHz = 1500, s_FakeTemp = 58, s_FakeWarn = 0;
	status_fake (0); sidecar_status_set (&s_Stv);
	EXPECT (text_shown (lv_screen_active (), o ? "1500 MHZ" : "1500 MHz"));
	sidecar_ui_show (0); run (2);
	{ int hx, hy; EXPECT (sidecar_ui_home_hit (8, &hx, &hy)); tap (hx, hy); }	// (Now: the pattern)
	EXPECT (sidecar_ui_page () == 13);

	// ---- PROJECT (project bundles): the home page's PROJECT panel opens it; the list, the stick, the bundles, delete's
	// confirmation (DELETE grey for a second), the progress with WAITING, the summary with a machine left out, done, failed
	sidecar_ui_show (0); run (2);
	snprintf (s_Last, sizeof s_Last, "none");
	tap_raw (SUBX (4, 2), SUBY);	// HOME > Project (the 3rd of 4 pills)
	EXPECT (sidecar_ui_page () == 14 && sidecar_project_shown () && !sidecar_status_shown ());
	EXPECT_CMD ("bundle=list:card");
	{
		project_fake (SPJ_IDLE); sidecar_project_set (&s_Pv); run (2);
		shot ("project");
		EXPECT (sidecar_project_selected () == 0 && sidecar_project_dialog () == 0);
		project_tap (SPJ_HIT_ROW, 1);
		EXPECT (sidecar_project_selected () == 1);
		project_tap (SPJ_HIT_KEY, 0); EXPECT_CMD ("bundle=export:card:LIVE SET 3");
		project_tap (SPJ_HIT_DEST, 1);
		shot ("project-usb");
		project_tap (SPJ_HIT_KEY, 0); EXPECT_CMD ("bundle=export:USB:LIVE SET 3");
		project_tap (SPJ_HIT_EJECT, 0); EXPECT_CMD ("bundle=eject:USB");
		project_tap (SPJ_HIT_KEY, 1); EXPECT_CMD ("bundle=list:card");
		EXPECT (sidecar_project_listing () == 1);
		shot ("project-bundles");
		snprintf (s_Last, sizeof s_Last, "none");
		project_tap (SPJ_HIT_ROW, 2);
		project_tap (SPJ_HIT_KEY, 0); EXPECT_CMD ("none");		// an incomplete one: not imported
		project_tap (SPJ_HIT_ROW, 0);
		project_tap (SPJ_HIT_KEY, 0); EXPECT_CMD ("bundle=verify:/Bundles/AR PERC PATCH 20261022-1403.sidecar.zip");
		project_tap (SPJ_HIT_KEY, 1);					// BACK
		EXPECT (sidecar_project_listing () == 0);
		snprintf (s_Last, sizeof s_Last, "none");
		project_tap (SPJ_HIT_ROW, 0);
		project_tap (SPJ_HIT_KEY, 2); EXPECT (sidecar_project_dialog () == 0);	// the current project: DELETE is off
		project_tap (SPJ_HIT_ROW, 2);
		project_tap (SPJ_HIT_KEY, 2);
		EXPECT (sidecar_project_dialog () == 6);
		shot ("project-delete");
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("none");		// grey for its first second
		project_tap (SPJ_HIT_KEY, 2);
		run (30);
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("bundle=delete:JAM 1021");
		EXPECT (sidecar_project_dialog () == 0);
		// 2026-10-27: TRASH lists the project trash; RESTORE puts the selected entry back; BACK
		project_tap (SPJ_HIT_KEY, 5); EXPECT_CMD ("bundle=list:card");
		EXPECT (sidecar_project_listing () == 2);
		project_tap (SPJ_HIT_ROW, 1);
		EXPECT (sidecar_project_selected () == 1);
		shot ("project-trash");
		project_tap (SPJ_HIT_KEY, 0); EXPECT_CMD ("bundle=restore:9");
		project_tap (SPJ_HIT_KEY, 1);					// BACK
		EXPECT (sidecar_project_listing () == 0);
		project_fake (SPJ_WRITE); sidecar_project_set (&s_Pv); run (2);
		EXPECT (sidecar_project_dialog () == 1);
		shot ("project-progress");
		project_tap (SPJ_HIT_DLG, 0); EXPECT_CMD ("bundle=cancel");
		project_fake (SPJ_READY); sidecar_project_set (&s_Pv); run (2);
		EXPECT (sidecar_project_dialog () == 2);
		project_tap (SPJ_HIT_MACH, 0);					// GRAIN left out
		shot ("project-summary");
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("bundle=import:1:1");
		project_fake (SPJ_DONE); sidecar_project_set (&s_Pv); run (2);
		EXPECT (sidecar_project_dialog () == 3);
		shot ("project-done");
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("bundle=finish");
		project_fake (SPJ_READY); s_Pv.plan_slot = 47;			// 2026-10-29 slot mode: slot 47 has files - the web page replaces
		snprintf (s_Pv.plan_step, sizeof s_Pv.plan_step, "Send AR PERC PATCH.arprj to the Rytm's slot 047 with Elektron Transfer, then load slot 047 on the Rytm.");
		sidecar_project_set (&s_Pv); run (2);
		shot ("project-summary-slot");
		s_Last[0] = 0;
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("");			// (IMPORT greyed: nothing sent)
		project_fake (SPJ_DONE); s_Pv.plan_slot = 47, s_Pv.plan_clash = 0;
		snprintf (s_Pv.plan_step, sizeof s_Pv.plan_step, "Send AR PERC PATCH.arprj to the Rytm's slot 047 with Elektron Transfer, then load slot 047 on the Rytm.");
		sidecar_project_set (&s_Pv); run (2);
		shot ("project-done-slot");
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("bundle=finish");
		project_fake (SPJ_READY); s_Pv.plan_slot = 1, s_Pv.plan_pick = 1, s_Pv.plan_clash = 0;	// every slot taken: picked on the web page
		snprintf (s_Pv.plan_step, sizeof s_Pv.plan_step, "Every slot holds a project: pick the slot this bundle's Sidecar side goes to.");
		sidecar_project_set (&s_Pv); run (2);
		shot ("project-summary-pick");
		s_Last[0] = 0;
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("");
		project_tap (SPJ_HIT_DLG, 0); EXPECT_CMD ("bundle=finish");
		project_fake (SPJ_FAILED); sidecar_project_set (&s_Pv); run (2);
		EXPECT (sidecar_project_dialog () == 4);
		shot ("project-failed");
		project_tap (SPJ_HIT_DLG, 1); EXPECT_CMD ("bundle=finish");
		project_fake (SPJ_IDLE); sidecar_project_set (&s_Pv); run (2);
		EXPECT (sidecar_project_dialog () == 0);
		{	// a progress update redraws a few labels and the bar (the whole page's redraw beside it)
			double full = bench (0, 5);
			project_fake (SPJ_WRITE); sidecar_project_set (&s_Pv); run (2);
			s_Pv.done += 1 << 20, s_Pv.seq++;
			double t0 = now_us ();
			sidecar_project_set (&s_Pv); lv_refr_now (NULL);
			printf ("project [%s]: a progress update %.0f us, the whole screen %.0f us (Mac)\n", s_Pre, now_us () - t0, full);
			project_fake (SPJ_IDLE); sidecar_project_set (&s_Pv); run (2);
		}
	}

	sidecar_ui_show (0);
	EXPECT (!sidecar_insfx_shown ());
	EXPECT (!sidecar_voice_shown ());
	EXPECT (!sidecar_wgview_list_shown ());

	// the controls: what a tap sends (UI coordinates per theme)
	sidecar_ui_show (0); run (3);
	if (o)
	{
		const int lw = LU (172) + DX / 2, rx = LU (176) + DX / 2, rw = LU (352) + DX - rx, cw = (LU (352) + DX - LU (4)) / 2;
		tap_raw (NAV_X, NAV_Y (7));							// SET: the settings (the last page: About)
		tap_raw (SUBX (6, 0), SUBY);								// tab: Sound
		tap (LU (44) + DX / 8, LU (41) + VS (24) + VS (22) / 2);         EXPECT_CMD ("notes=0");	// ENGINE panel at y 41: notes Off
		tap (LU (127) + 3 * DX / 8, LU (41) + VS (63) + VS (22) / 2);   EXPECT_CMD ("out=tracks");
		tap (LU (127) + 3 * DX / 8, LU (41) + VS (102) + VS (22) / 2);  EXPECT_CMD ("mac=on");
		tap (LU (127) + 3 * DX / 8, LU (41) + VS (141) + VS (22) / 2);  EXPECT_CMD ("usbmode=overbridge");	// the USB-C mode
		tap (LU (44) + DX / 8, LU (41) + VS (141) + VS (22) / 2);       EXPECT_CMD ("usbmode=class");
		tap (rx + LU (4) + (rw - LU (8)) * 7 / 8, LU (41) + VS (13) + VS (46) / 4);				// BT = voice 1 (row 1, 4th: the Rytm's track order)
		tap (rx + LU (4) + (rw - LU (8)) * 5 / 6, LU (41) + VS (76) + VS (46) * 3 / 4);  EXPECT_CMD ("notemach=1:md");	// machine MD (row 2, last; DIGIDRUM before 0.3.0)
		tap_raw (SUBX (6, 1), SUBY);							// tab: MIDI (0.5.0): the keyboards' route, auto channel, computer notes
		EXPECT (sidecar_ui_page () == 23);
		tap (LU (4) + (lw - LU (8)) * 3 / 4, LU (41) + VS (24) + VS (22) / 2);   EXPECT_CMD ("keys=channel");
		tap (LU (4) + (lw - LU (8)) / 4, LU (41) + VS (24) + VS (22) / 2);       EXPECT_CMD ("keys=selected");
		tap (LU (22), LU (41) + VS (63) + VS (22) / 2);                          EXPECT_CMD ("keys=auto:13");
		sidecar_ui_status (&s_St);						// review L3: a state read before core 3 applied it (14)
		tap (LU (22), LU (41) + VS (63) + VS (22) / 2);                          EXPECT_CMD ("keys=auto:12");
		s_St.keys_auto = 12; sidecar_ui_status (&s_St); s_St.keys_auto = 14;	// the engine's state matches: the local value goes
		tap (lw - LU (22), LU (41) + VS (63) + VS (22) / 2);                     EXPECT_CMD ("keys=auto:13");
		s_St.keys_auto = 13; sidecar_ui_status (&s_St); s_St.keys_auto = 14;
		tap (lw - LU (22), LU (41) + VS (63) + VS (22) / 2);                     EXPECT_CMD ("keys=auto:14");
		sidecar_ui_status (&s_St);
		tap (LU (4) + (lw - LU (8)) / 4, LU (41) + VS (102) + VS (22) / 2);      EXPECT_CMD ("keys=mac:off");
		tap (LU (4) + (lw - LU (8)) * 3 / 4, LU (41) + VS (102) + VS (22) / 2);  EXPECT_CMD ("keys=mac:on");
		tap_raw (SUBX (6, 4), SUBY);							// tab: Service (T18): FIFO, the drone
		EXPECT (sidecar_ui_page () == 16);
		run (2); shot ("service");
		tap (LU (22), LU (41) + VS (24) + VS (24) / 2);                 EXPECT_CMD ("fifo=88");
		tap (LU (182), LU (41) + VS (24) + VS (24) / 2);                EXPECT_CMD ("fifo=96");
		tap (LU (4) + (LU (352) + DX - LU (12)) / 6, LU (41) + VS (74) + VS (22) / 2);         EXPECT_CMD ("drone=off");
		tap (LU (4) + (LU (352) + DX - LU (12)) / 2, LU (41) + VS (74) + VS (22) / 2);         EXPECT_CMD ("drone=1");
		tap (LU (4) + (LU (352) + DX - LU (12)) * 5 / 6, LU (41) + VS (74) + VS (22) / 2);     EXPECT_CMD ("drone=all");
		tap (LU (4) + (LU (352) + DX - LU (12)) / 6, LU (41) + VS (141) + VS (22) / 2);        EXPECT_CMD ("cpumhz=1500");	// the CPU clock option
		tap (LU (4) + (LU (352) + DX - LU (12)) / 2, LU (41) + VS (141) + VS (22) / 2);        EXPECT_CMD ("cpumhz=1200");
		tap (LU (4) + (LU (352) + DX - LU (12)) * 5 / 6, LU (41) + VS (141) + VS (22) / 2);    EXPECT_CMD ("cpumhz=1000");
		run (2); shot ("system-1000");
		sidecar_ui_show (2); run (2);							// FX > Sends
		tap (LU (266) + DX + LU (40), LU (41) + LU (14) + LU (12));  EXPECT_CMD ("killfx");
		tap_raw (NAV_X, NAV_Y (7));							// SET: its last page, then the Display pill
		tap_raw (SUBX (6, 2), SUBY);							// tab: Display
		tap (LU (84) + (LU (262) + DX) / 8, LU (41) + VS (43) + VS (24) / 2);      EXPECT_CMD ("screensaver=0");
		tap (LU (84) + (LU (262) + DX) * 5 / 8, LU (41) + VS (43) + VS (24) / 2);  EXPECT_CMD ("screensaver=60");
		(void) cw;
		tap_raw (NAV_X, NAV_Y (0));							// HOME
		shot ("hidden");
		sidecar_ui_show (6); run (3);
		unsigned taps = sidecar_ui_taps ();
		tap (LU (84) + LU (30), LU (41) + VS (73) + VS (24) / 2);  EXPECT (sidecar_ui_taps () == taps + 1);	// Display's touch test: TAP
	}
	else
	{
		const int x0 = LU (12) + LU (400) + DX / 2 + LU (12), vw = LU (364) + DX / 2, by = LU (308) + PDY / 2, e = PDY / 6, lw = LU (380) + DX / 2;
		tap_raw (CNAV_X (7), CNAV_Y (7));							// SET (the last page: About)
		tap_raw (SUBX (6, 0), SUBY);								// tab: Sound
		tap (LU (100), LU (110) + CY (48) + CH / 2);                 EXPECT_CMD ("notes=0");
		tap (LU (300) + 3 * DX / 8, LU (110) + CY (118) + CH / 2);   EXPECT_CMD ("out=tracks");
		tap (LU (300) + 3 * DX / 8, LU (110) + CY (188) + CH / 2);   EXPECT_CMD ("mac=on");
		tap (LU (300) + 3 * DX / 8, LU (110) + CY (262) + CH / 2);   EXPECT_CMD ("usbmode=overbridge");	// the USB-C mode
		tap (LU (100), LU (110) + CY (262) + CH / 2);                EXPECT_CMD ("usbmode=class");
		tap (x0 + LU (140) + 7 * DX / 32, LU (110) + CY (48) + CH / 2);			// BT = voice 1 (4th key: the Rytm's track order)
		tap (x0 + vw - LU (20), LU (110) + CY (118) + CH / 2);   EXPECT_CMD ("notemach=1:md");
		tap_raw (SUBX (6, 1), SUBY);								// tab: MIDI (0.5.0)
		EXPECT (sidecar_ui_page () == 23);
		{
			const int mw = LU (354) + DX / 2, cy = LU (110);
			tap (LU (12) + LU (12) + mw * 3 / 4, cy + CY (48) + CH / 2);    EXPECT_CMD ("keys=channel");
			tap (LU (12) + LU (12) + mw / 4, cy + CY (48) + CH / 2);        EXPECT_CMD ("keys=selected");
			tap (LU (12) + LU (12) + LU (45), cy + CY (118) + CH / 2);      EXPECT_CMD ("keys=auto:13");
			sidecar_ui_status (&s_St);					// review L3: a stale state read (14) between taps
			tap (LU (12) + LU (12) + LU (45), cy + CY (118) + CH / 2);      EXPECT_CMD ("keys=auto:12");
			s_St.keys_auto = 12; sidecar_ui_status (&s_St); s_St.keys_auto = 14;
			tap (LU (12) + LU (274) + DX / 2 + LU (45), cy + CY (118) + CH / 2);  EXPECT_CMD ("keys=auto:13");
			s_St.keys_auto = 13; sidecar_ui_status (&s_St); s_St.keys_auto = 14;
			tap (LU (12) + LU (274) + DX / 2 + LU (45), cy + CY (118) + CH / 2);  EXPECT_CMD ("keys=auto:14");
			sidecar_ui_status (&s_St);
			tap (LU (12) + LU (12) + mw / 4, cy + CY (188) + CH / 2);       EXPECT_CMD ("keys=mac:off");
			tap (LU (12) + LU (12) + mw * 3 / 4, cy + CY (188) + CH / 2);   EXPECT_CMD ("keys=mac:on");
		}
		tap_raw (SUBX (6, 4), SUBY);								// tab: Service (T18)
		EXPECT (sidecar_ui_page () == 16);
		run (2); shot ("service");
		tap (LU (12) + LU (12) + LU (48), LU (110) + CY (48) + CH / 2);            EXPECT_CMD ("fifo=88");
		tap (LU (12) + LU (270) + LU (48), LU (110) + CY (48) + CH / 2);           EXPECT_CMD ("fifo=96");
		tap (LU (12) + LU (12) + LU (40), LU (110) + CY (138) + CH / 2);           EXPECT_CMD ("drone=off");
		tap (LU (12) + LU (12) + LU (182), LU (110) + CY (138) + CH / 2);          EXPECT_CMD ("drone=1");
		tap (LU (12) + LU (12) + LU (364) - LU (30), LU (110) + CY (138) + CH / 2);     EXPECT_CMD ("drone=all");
		tap (LU (12) + LU (12) + LU (40), LU (110) + CY (266) + CH / 2);           EXPECT_CMD ("cpumhz=1500");	// the CPU clock option
		tap (LU (12) + LU (12) + LU (182), LU (110) + CY (266) + CH / 2);          EXPECT_CMD ("cpumhz=1200");
		tap (LU (12) + LU (12) + LU (364) - LU (30), LU (110) + CY (266) + CH / 2);     EXPECT_CMD ("cpumhz=1000");
		run (2); shot ("system-1000");
		sidecar_ui_show (2); run (2);							// FX > Sends
		{	// (the Kill tails card: the third of the cards' row)
			const int kw = LU (200), cw = (LU (776) + DX - kw - 2 * LU (8)) / 2;
			tap (LU (12) + 2 * (cw + LU (8)) + kw / 2, LU (110) + (LU (118) + DY / 6) / 2);  EXPECT_CMD ("killfx");
		}
		tap_raw (CNAV_X (7), CNAV_Y (7));							// SET: its last page, then the Display pill
		tap_raw (SUBX (6, 2), SUBY);								// tab: Display
		tap (LU (190) + LU (60), LU (110) + CY (78) + CH / 2);                      EXPECT_CMD ("screensaver=0");
		tap (LU (190) + (LU (570) + DX) * 5 / 8 + LU (4), LU (110) + CY (78) + CH / 2);  EXPECT_CMD ("screensaver=60");
		(void) by; (void) e; (void) lw;
		tap_raw (CNAV_X (0), CNAV_Y (0));							// HOME
		sidecar_ui_show (0); run (3);
		shot ("hidden");
		sidecar_ui_show (6); run (3);
		unsigned taps = sidecar_ui_taps ();
		tap (LU (190) + LU (55), LU (110) + CY (126) + CH / 2);  EXPECT (sidecar_ui_taps () == taps + 1);	// Display's touch test: TAP
	}
	// N1: the eight navigation keys in both themes - each opens its page and lights itself; VOICE asks for the Rytm's
	// track again (voicepick=-1).
	// rui1052: and the caption painted in each key's corner is that page's own key on the Rytm, so the column reads
	// T1..T8 top to bottom - the sidebar and UP + T1..T8 in one order (core/ui/page_keys.h)
	{
		static const int kPg[8] = {0, 4, 5, 8, 2, 10, 11, -1};
		static const char *const kKey[8] = {"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8"};
		static const char *const kPage[8] = {"HOME", "SEQ", "PAT", "VOICE", "FX", "SONG", "REC", "SET"};
		sidecar_ui_show (2); sidecar_ui_show (8); sidecar_ui_show (0); run (2);		// (FX remembers Sends, VOICE Sound)
		for (int k = 0; k < 8; k++)
		{
			EXPECT (!strcmp (sidecar_ui_nav_page (k), kPage[k]));
			EXPECT (!strcmp (sidecar_ui_nav_key (k), kKey[k]));
			if (o) tap_raw (NAV_X, NAV_Y (k)); else tap_raw (CNAV_X (k), CNAV_Y (k));
			if (kPg[k] >= 0) EXPECT (sidecar_ui_page () == kPg[k]);
			else EXPECT (sidecar_ui_page () == 1 || sidecar_ui_page () == 2 || sidecar_ui_page () == 3 || sidecar_ui_page () == 6 || sidecar_ui_page () == 16 || sidecar_ui_page () == 23);
			EXPECT (sidecar_ui_nav_lit () == k);
			if (k == 3) EXPECT_CMD ("voicepick=-1");
		}
		sidecar_ui_show (12); EXPECT (sidecar_ui_nav_lit () == 3);	// SCOPE: under VOICE
		sidecar_ui_show (13); EXPECT (sidecar_ui_nav_lit () == 0);	// STATUS: under HOME
		sidecar_ui_show (0); run (2);
	}
	// 0.5.1: the sections and their sub-page rows: a pill opens its page and lights its section's key, a section's key lands
	// on the pill used last, SEQ and PAT carry their own row (no pill row of the section)
	{
#define NAVTAP(k)	do { if (o) tap_raw (NAV_X, NAV_Y (k)); else tap_raw (CNAV_X (k), CNAV_Y (k)); } while (0)
#define PILL(n, i)	tap_raw (SUBX (n, i), SUBY)
		const int nfx = 3 + SIDECAR_MORPH + SIDECAR_LOOPER;	// Sends Master [Morph] XY [Loop]
		sidecar_ui_show (8); sidecar_ui_show (2); sidecar_ui_show (0); run (2);	// (VOICE remembers Sound, FX Sends)
		PILL (3, 1); EXPECT (sidecar_ui_page () == 13 && sidecar_ui_nav_lit () == 0);		// HOME: Status
		PILL (3, 2); EXPECT (sidecar_ui_page () == 14 && sidecar_project_shown ());		// Project
		PILL (3, 0); EXPECT (sidecar_ui_page () == 0);						// Overview: today's home page
		PILL (3, 2);
		NAVTAP (3); EXPECT (sidecar_ui_page () == 8 && sidecar_ui_nav_lit () == 3);			// VOICE: Sound
		PILL (4, 1); EXPECT (sidecar_ui_page () == 9 && sidecar_insfx_shown ());			// Insert
		PILL (4, 2); EXPECT (sidecar_ui_page () == 7 && sidecar_wgview_shown ());			// Source
		PILL (4, 3); EXPECT (sidecar_ui_page () == 12 && sidecar_scope_shown ());			// Scope
		NAVTAP (0); EXPECT (sidecar_ui_page () == 14);						// HOME remembers Project
		NAVTAP (3); EXPECT (sidecar_ui_page () == 12);						// VOICE remembers Scope
		PILL (4, 0); EXPECT (sidecar_ui_page () == 8); EXPECT_CMD ("voicepick=-1");		// Sound: the Rytm's track again
		NAVTAP (4); EXPECT (sidecar_ui_page () == 2 && sidecar_ui_nav_lit () == 4);			// FX: Sends
		PILL (nfx, 1); EXPECT (sidecar_ui_page () == 19 && sidecar_mfx_shown ());			// Master
		PILL (nfx, nfx - 1 - SIDECAR_LOOPER); EXPECT (sidecar_ui_page () == 22 && sidecar_xy_shown ());		// XY
		NAVTAP (7); NAVTAP (4); EXPECT (sidecar_ui_page () == 22);					// FX remembers XY
		PILL (nfx, 0); EXPECT (sidecar_ui_page () == 2);
		NAVTAP (5); PILL (2, 1); EXPECT (sidecar_ui_page () == 10 && sidecar_song_explain_shown ());	// SONG: Setup (the how-to)
		NAVTAP (0); NAVTAP (5); EXPECT (sidecar_ui_page () == 10 && sidecar_song_explain_shown ());	// remembered
		PILL (2, 0); EXPECT (!sidecar_song_explain_shown ());					// Timeline
		NAVTAP (6); PILL (2, 1); EXPECT (sidecar_ui_page () == 11 && sidecar_rec_takes_shown ());	// REC: Takes
		PILL (2, 0); EXPECT (!sidecar_rec_takes_shown ());					// Record
		{
			static const int kSetPg[6] = {1, 23, 6, 15, 16, 3};
			NAVTAP (7);
			for (int i = 0; i < 6; i++)
			{
				PILL (6, i); EXPECT (sidecar_ui_page () == kSetPg[i] && sidecar_ui_nav_lit () == 7);
				if (i == 3) { EXPECT (sidecar_wgview_pool_page_shown ()); }
			}
			NAVTAP (0); NAVTAP (7); EXPECT (sidecar_ui_page () == 3);				// SET remembers About
		}
		NAVTAP (1); EXPECT (sidecar_ui_page () == 4);						// SEQ, PAT: their own header row
		NAVTAP (2); EXPECT (sidecar_ui_page () == 5);
		sidecar_ui_show (2); sidecar_ui_show (8); sidecar_ui_show (0); run (2);
#undef PILL
#undef NAVTAP
	}
	// L1: the STAGE look (OLED): whole dots, lighter greys; the Display page's LOOK key sends uilook=, the stored value
	// (status ui_look) sets it; STUDIO back
	if (o)
	{
		s_St.ui_look = 1; sidecar_ui_status (&s_St);
		EXPECT (sidecar_dot_get_look () == 1);
		sidecar_ui_show (0); run (2); shot ("stage-home");
		sidecar_ui_show (13); run (2); shot ("stage-status");
		sidecar_ui_show (6); run (2); shot ("stage-display");
		tap (LU (84) + (LU (262) + DX) / 4, LU (41) + VS (136) + VS (24) / 2); EXPECT_CMD ("uilook=studio");
		EXPECT (sidecar_dot_get_look () == 0);
		tap (LU (84) + (LU (262) + DX) * 3 / 4, LU (41) + VS (136) + VS (24) / 2); EXPECT_CMD ("uilook=stage");
		tap (LU (84) + (LU (262) + DX) / 4, LU (41) + VS (136) + VS (24) / 2); EXPECT_CMD ("uilook=studio");
		s_St.ui_look = 0; sidecar_ui_status (&s_St);
		sidecar_ui_show (0); run (2);
	}
	// F2: a mirror page (VOICE) while the Rytm is not connected says it shows the last it had
	s_St.rytm = 0; sidecar_ui_status (&s_St);
	sidecar_ui_show (8); run (40); shot ("voice-no-rytm"); alert_bars (17);	// (after the link banner; the header's "No Rytm link")
	s_St.rytm = 1; sidecar_ui_status (&s_St);
	sidecar_ui_show (0); run (2);
	// N3: the Rytm's UP held alone - the UP + T map over the screen, gone at the release
	sidecar_ui_combo_map (1); run (2); shot ("combo-map");
	EXPECT (sidecar_ui_combo_map_shown ());
	// rui1046 / rui1052: the map from the device's table (core/ui/page_keys.h) - every page request in the table's
	// order, then the UP + key combos (the glass lists all 13: the Rytm's own map folds the ones only it has)
	EXPECT (text_shown (lv_layer_top (), style == THEME_OLED ? "T13 POOL" : "T13 Pool"));
	EXPECT (text_shown (lv_layer_top (), "TRIG SEQ"));
	EXPECT (text_shown (lv_layer_top (), style == THEME_OLED ? "LFO LFOS" : "LFO LFOs"));
	EXPECT (text_shown (lv_layer_top (), style == THEME_OLED ? "T5 FX" : "T5 FX"));
	EXPECT (text_shown (lv_layer_top (), style == THEME_OLED ? "T6 SONG" : "T6 Song"));
	EXPECT (text_shown (lv_layer_top (), "T7 REC"));
	EXPECT (text_shown (lv_layer_top (), "T8 SET"));
	EXPECT (text_shown (lv_layer_top (), style == THEME_OLED ? "T11 SAMPLE" : "T11 Sample"));
	sidecar_ui_combo_map (0); run (2);
	EXPECT (!sidecar_ui_combo_map_shown ());
	// the power warning (power_state.h's short text, red), over the title bar
	status_init (style);
	s_St.note = "POWER TOO WEAK: use 15 W supply";	// (power_state.cpp short_warning)
	sidecar_ui_banner_reopen ();				// (a boot of its own: the other theme's run closed it)
	sidecar_ui_status (&s_St);
	sidecar_ui_show (0); run (80); shot ("power");		// (run: past the Rytm's connect animation, which covers the banner)
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), "POWER TOO WEAK"));
	EXPECT (text_shown (lv_screen_active (), o ? "POWER TOO WEAK" : "power too weak"));	// HOME's Health line says it too
	// its x closes it for this boot: the same text stays away (the status each second, a rebuild), Health keeps saying it;
	// another text shows it again with its x
	{
		int px, py;
		EXPECT (sidecar_ui_banner_close (&px, &py));
		tap (px, py); run (2); shot ("power-closed");
		EXPECT (sidecar_ui_banner () == 0 && !sidecar_ui_banner_close (0, 0));
		EXPECT (sidecar_ui_page () == 0);			// the tap reached only the x
		EXPECT (text_shown (lv_screen_active (), o ? "POWER TOO WEAK" : "power too weak"));
		sidecar_ui_status (&s_St); run (4);
		EXPECT (sidecar_ui_banner () == 0);
		s_St.note = "POWER TOO WEAK: use 27 W adapter";
		sidecar_ui_status (&s_St); run (4);
		EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), "27 W") && sidecar_ui_banner_close (0, 0));
		EXPECT (sidecar_ui_banner_close (&px, &py));
		tap (px, py); run (2);
		EXPECT (sidecar_ui_banner () == 0);
		s_St.temp_c = 81, s_St.temp_warn = 1;			// closed: a temperature warning shows (no x: it wants a look)
		sidecar_ui_status (&s_St); run (4);
		EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "SOC 81C" : "SoC 81") && !sidecar_ui_banner_close (0, 0));
		s_St.temp_c = 50, s_St.temp_warn = 0, s_St.note = "POWER TOO WEAK: use 15 W supply";
		sidecar_ui_banner_reopen ();
		sidecar_ui_status (&s_St); run (4);
		EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), "POWER TOO WEAK"));
	}
	// the temperature warning's banner (thermal.h): the same banner; on, off, and under the supply's warning (that wins)
	status_init (style);
	run (80);
	s_St.temp_c = 76, s_St.temp_warn = 1;
	sidecar_ui_status (&s_St);
	sidecar_ui_show (0); run (80); shot ("hot");
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "SOC 76C TOO HOT" : "SoC 76 \xC2\xB0" "C - too hot"));
	s_St.temp_c = 74;							// 74 C: still warned
	sidecar_ui_status (&s_St);
	run (4);
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "SOC 74C TOO HOT" : "SoC 74 \xC2\xB0" "C - too hot"));
	s_St.cpu_mhz = 1000;							// the clock as low as it goes: the case fan is the other lever
	sidecar_ui_status (&s_St);
	run (4); shot ("hot-1000");
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "CASE FAN" : "case fan"));
	s_St.cpu_mhz = 1500;
	sidecar_ui_status (&s_St);
	run (4);
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "SYSTEM > CLOCK" : "Set > System > clock"));
	// 0.3.0: Circle's throttle lowered the clock (600 MHz): the banner says so; under the warning (79 C) no banner at all
	s_St.temp_c = 81, s_St.clock_low_mhz = 600;
	sidecar_ui_status (&s_St);
	run (4); shot ("hot-lowered");
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "SOC 81C - CLOCK LOWERED TO 600 MHZ" : "SoC 81 \xC2\xB0" "C - clock lowered to 600 MHz."));
	s_St.temp_c = 79, s_St.temp_warn = 0;
	sidecar_ui_status (&s_St);
	run (4);
	EXPECT (sidecar_ui_banner () == 0);
	s_St.temp_c = 74, s_St.temp_warn = 1, s_St.clock_low_mhz = 0;
	sidecar_ui_status (&s_St);
	run (4);
	s_St.note = "POWER TOO WEAK: use 15 W supply";				// both: the supply's text
	sidecar_ui_status (&s_St);
	run (4); shot ("hot-and-power");
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), "POWER TOO WEAK") && !strstr (sidecar_ui_banner (), "HOT") && !strstr (sidecar_ui_banner (), "hot"));
	s_St.note = "link: usb", s_St.temp_c = 69, s_St.temp_warn = 0;		// 69 C: cleared
	sidecar_ui_status (&s_St);
	run (4); shot ("hot-cleared");
	EXPECT (sidecar_ui_banner () == 0);
	// 0.5.0 LFO FINAL: the Rytm link full - the same banner, under the temperature's (that wins), gone with it
	s_St.lfo_full = 1;
	sidecar_ui_status (&s_St);
	run (4); shot ("lfo-full");
	EXPECT (sidecar_ui_banner () && strstr (sidecar_ui_banner (), o ? "RYTM LINK FULL: LFOS UPDATE SLOWER" : "RYTM LINK FULL: LFOs update slower"));
	s_St.temp_c = 81, s_St.temp_warn = 1;
	sidecar_ui_status (&s_St);
	run (4);
	EXPECT (sidecar_ui_banner () && !strstr (sidecar_ui_banner (), "RYTM LINK"));
	s_St.temp_c = 69, s_St.temp_warn = 0, s_St.lfo_full = 0;
	sidecar_ui_status (&s_St);
	run (4);
	EXPECT (sidecar_ui_banner () == 0);
	status_init (style);
	s_St.touch_hidden = 1;
	sidecar_ui_status (&s_St);
	sidecar_ui_show (0); shot ("home-info");

	// ---- SEQ: the touch pattern editor on a core/seq pattern (seqdemo.cpp), every edit a "seqed=" line
	seqdemo_build ();
	s_SeqPlay = 1;
	seqdemo_view (&s_View, 1);
	sidecar_seqed_view (&s_View);
	sidecar_ui_show (0); run (1);
	sidecar_ui_show (4);
	int top = o ? 77 : 76;
	sidecar_seqed_debug (SEQED_DRAW, 0, 2, top, -1, -1);
	sidecar_seqed_playhead (13400);
	shot ("seq-roll");
	int gx, gy, gw, gh, rh, ly, lh;
	sidecar_seqed_geom (&gx, &gy, &gw, &gh, &rh, &ly, &lh);
#define SX(st, k) (gx + (st) * gw / (16 * (k)) + gw / (32 * (k)))
#define PY(p, t) (gy + ((t) - (p)) * rh + rh / 2)
	// T5: opened while the Rytm plays - VIEW: a tap edits nothing (a toast says why); DRAW edits, a new note then takes
	// a long press while playing (a plain tap: a toast); stopped, a tap edits
	sidecar_seqed_debug (SEQED_DRAW, 0, 1, top, -1, -1);
	run (3);
	EXPECT (sidecar_seqed_locked ());
	s_nCmds = 0;
	tap (SX (9, 1), PY (65, top));
	EXPECT (s_nCmds == 0 && seqdemo_note (8, 9, 65, 0, 0) == 0 && !strcmp (sidecar_seqed_toast (), "VIEW: TAP DRAW TO EDIT"));
	run (2); shot ("seq-view");
	{
		int tx, ty;
		sidecar_seqed_tool_xy (SEQED_DRAW, &tx, &ty);
		tap (tx, ty);
	}
	EXPECT (!sidecar_seqed_locked ());
	tap (SX (9, 1), PY (65, top));
	EXPECT (s_nCmds == 0 && !strcmp (sidecar_seqed_toast (), "HOLD TO ADD (PLAYING)"));
	hold (SX (9, 1), PY (65, top));
	EXPECT (seqdemo_note (8, 9, 65, 0, 0) > 0 && !strncmp (sidecar_seqed_toast (), "+ F4 S10", 8));
	run (2); shot ("seq-toast");
	hold (SX (9, 1) - (o ? LU (4) : LU (10)), PY (65, top));					// selected: a long press deletes
	EXPECT (seqdemo_note (8, 9, 65, 0, 0) == 0 && !strncmp (sidecar_seqed_toast (), "- F4 S10", 8));
	s_SeqPlay = 0; seqdemo_view (&s_View, 0); sidecar_seqed_view (&s_View);		// stopped: taps edit
	// gestures on one page: a tap adds a note, a tap on it (selected) deletes it, a drag moves one, UNDO
	sidecar_seqed_playhead (-1);
	sidecar_seqed_debug (SEQED_DRAW, 0, 1, top, -1, -1);
	run (3);
	tap (SX (9, 1), PY (65, top));
	EXPECT (seqdemo_note (8, 9, 65, 0, 0) > 0);
	tap (SX (9, 1) - (o ? LU (4) : LU (10)), PY (65, top));
	EXPECT (seqdemo_note (8, 9, 65, 0, 0) == 0);
	int m0 = 0;
	EXPECT (seqdemo_note (8, 2, 72, 0, &m0) > 0);
	int nx = gx + (2 * 96 + m0) * gw / 1536 + gw / 64;
	drag (nx, PY (72, top), nx + gw / 16, PY (73, top), 6, 0);		// one step later, a semitone up: micro kept
	int m1 = 99;
	EXPECT (seqdemo_note (8, 2, 72, 0, 0) == 0 && seqdemo_note (8, 3, 73, 0, &m1) > 0 && m1 == m0);
	if (o) tap (LU (278) + 5 * DX / 7 + (LU (35) + DX / 7) / 2, LU (218) + DY + LU (11)); else tap (LU (722) + DX, LU (52) + LU (400) + DY);	// UNDO
	EXPECT (seqdemo_note (8, 2, 72, 0, 0) > 0 && seqdemo_note (8, 3, 73, 0, 0) == 0);
	// editing a note: selected, dragged later and up (where it was: an outline), its card
	sidecar_seqed_debug (SEQED_DRAW, 0, 1, top + 1, 13, 74);
	sidecar_seqed_debug_drag (14, 6, 75, 144);
	sidecar_seqed_playhead (5200);
	shot ("seq-edit");
	sidecar_seqed_debug_drag (-1, 0, 0, 0);
	{	// U1 NOTE EDIT on the Rytm (seqed_pi publishes core/seq note_edit): its highlighted note - page 2's first - selected
		// here with its card, the window paged to it; the Rytm's selection (that step's notes) framed a second time
		seqdemo_view (&s_View, 0);
		int k = -1;
		for (int i = 0; i < s_View.n; i++)
			if (s_View.note[i].step >= 16 && (k < 0 || s_View.note[i].step < s_View.note[k].step
							  || (s_View.note[i].step == s_View.note[k].step && s_View.note[i].pitch > s_View.note[k].pitch))) k = i;
		EXPECT (k >= 0);
		s_View.ne_on = 1, s_View.ne_step = s_View.note[k].step, s_View.ne_pitch = s_View.note[k].pitch, s_View.ne_gen = 7, s_View.ne_sel = 0;
		for (int i = 0; i < s_View.n; i++)
			if (s_View.note[i].step == s_View.ne_step || s_View.note[i].step == s_View.ne_step + 3) s_View.note[i].flags |= 2, s_View.ne_sel++;
		sidecar_seqed_debug (SEQED_DRAW, 0, 1, top, -1, -1);
		sidecar_seqed_view (&s_View);
		run (40);					// (the toast of the move above gone)
		EXPECT (sidecar_seqed_first_page () == 1);
		shot ("seq-noteedit");
		seqdemo_view (&s_View, 0);			// NOTE EDIT left: back to the editor's own
		sidecar_seqed_view (&s_View);
		sidecar_seqed_debug (SEQED_DRAW, 0, 1, top, -1, -1);
	}
	// the chord stamp: F MIN7, root position, broken by 12/96 a note, tapped on page 2 step 7 (F4 row)
	sidecar_seqed_debug (SEQED_CHORD, 1, 1, top, -1, -1);
	sidecar_seqed_debug_key (0, 2, 7, 0, 2);
	run (3);
	tap (SX (6, 1), PY (65, top));
	int mc = -1;
	EXPECT (seqdemo_note (8, 22, 65, 0, 0) && seqdemo_note (8, 22, 68, 0, 0) && seqdemo_note (8, 22, 72, 0, 0)
		&& seqdemo_note (8, 22, 75, 0, &mc) && mc == 36);
	sidecar_seqed_playhead (22700);
	shot ("seq-chord");
	sidecar_seqed_debug (SEQED_STRUM, 0, 2, top, -1, -1);
	shot ("seq-strum");
	sidecar_seqed_debug (SEQED_HUMAN, 0, 2, top, -1, -1);
	shot ("seq-human");
	sidecar_seqed_debug (SEQED_KEY, 0, 2, top, -1, -1);
	shot ("seq-key");
	// touch2: SWING (the shown track's own, T9): a preset, + and -, straight again; the header shows it
	sidecar_seqed_debug (SEQED_SWING, 0, 2, top, -1, -1);
	run (3);
	{
		int lw = o ? LU (352) + DX : LU (684) + DX, lyy = o ? LU (171) + DY * 2 / 3 : LU (52) + LU (354) + DY * 2 / 3, lhh = o ? LU (44) + DY - DY * 2 / 3 : LU (70) + DY - DY * 2 / 3;
		int kyy = o ? lyy + LU (12) + (lhh - LU (12)) / 2 : lyy + LU (20) + (lhh - LU (70)) / 4 + (LU (48) + (lhh - LU (70)) / 2) / 2;
#define SLX(v) (o ? (v) * lw / 352 : LU (8) + (v) * lw / 684)
		int s0 = o ? SLX (37) : SLX (92), sw8 = o ? 278 * lw / 352 : 500 * lw / 684;
		tap (s0 + sw8 * 5 / 16, kyy); EXPECT_CMD ("seq=swing:9:58"); EXPECT (seqdemo_swing (8) == 58);	// preset 58
		tap (o ? SLX (318) + (lw - SLX (318)) / 2 : SLX (600) + (lw - LU (4) - (600 * lw / 684)) / 2, kyy);	// +
		EXPECT_CMD ("seq=swing:9:59"); EXPECT (seqdemo_swing (8) == 59);
		tap (o ? SLX (17) : LU (8) + LU (4) + 40 * lw / 684, kyy); tap (o ? SLX (17) : LU (8) + LU (4) + 40 * lw / 684, kyy);		// - -
		EXPECT_CMD ("seq=swing:9:57"); EXPECT (seqdemo_swing (8) == 57);
		tap (s0 + sw8 / 16, kyy); EXPECT_CMD ("seq=swing:9:50"); EXPECT (seqdemo_swing (8) == 50);	// straight
		tap (s0 + sw8 * 7 / 16, kyy); EXPECT (seqdemo_swing (8) == 62);
#undef SLX
	}
	shot ("seq-swing");
	// seqx (E0): POLY (the shown track's poly config, T9): VOICES 4, ALLOCATION ROTATE, UNISON 2 - each a command line,
	// the model follows, the info line what it resolves to
	sidecar_seqed_debug (SEQED_POLY, 0, 2, top, -1, -1);
	run (3);
	{
		int lw = o ? LU (352) + DX : LU (684) + DX, lyy = o ? LU (171) + DY * 2 / 3 : LU (52) + LU (354) + DY * 2 / 3, lhh = o ? LU (44) + DY - DY * 2 / 3 : LU (70) + DY - DY * 2 / 3;
		int kyy = o ? lyy + LU (12) + (lhh - LU (12)) / 2 : lyy + LU (20) + (lhh - LU (70)) / 4 + (LU (48) + (lhh - LU (70)) / 2) / 2;
#define SLX(v) (o ? (v) * lw / 352 : LU (8) + (v) * lw / 684)
		int v0 = o ? 0 : LU (8) + LU (4), vw = o ? 190 * lw / 352 : 320 * lw / 684;			// VOICES: 7 keys
		int a0 = o ? SLX (194) : SLX (330), aw = o ? SLX (290) - SLX (194) : 196 * lw / 684;	// ALLOCATION: 3
		int u0 = o ? SLX (294) : SLX (538), uw = o ? lw - SLX (294) : lw - LU (4) - 538 * lw / 684;	// UNISON: 4
		tap (v0 + vw * 9 / 14, kyy); EXPECT_CMD ("seq=poly:9:voices=4"); EXPECT ((seqdemo_poly (8) & 15) == 4);
		tap (a0 + aw / 2, kyy); EXPECT_CMD ("seq=poly:9:alloc=rotate"); EXPECT ((seqdemo_poly (8) >> 4 & 3) == 1);
		tap (u0 + uw * 3 / 8, kyy); EXPECT_CMD ("seq=poly:9:unison=2"); EXPECT ((seqdemo_poly (8) >> 6) == 2);
#undef SLX
	}
	shot ("seq-poly");
	// seqtop: KEY (the pattern's QNT and seed), PLAY (T9's speed, direction, random notes), STEP (the selected note's
	// step 14: a ratchet, FX 1, its locks' condition, random notes) - each a command line, the model follows
	{
		int lw = o ? LU (352) + DX : LU (684) + DX, lyy = o ? LU (171) + DY * 2 / 3 : LU (52) + LU (354) + DY * 2 / 3, lhh = o ? LU (44) + DY - DY * 2 / 3 : LU (70) + DY - DY * 2 / 3;
		int rh = (lhh - LU (22)) / 2, y1 = o ? lyy + (21 * lhh / 44) / 2 : lyy + LU (18) + rh / 2, y2 = o ? lyy + 23 * lhh / 44 + (21 * lhh / 44) / 2 : lyy + LU (18) + rh + 2 + rh / 2;
#define SLX(v) (o ? (v) * lw / 352 : LU (8) + (v) * lw / 684)
#define KEYAT(x0, w, i, n) ((x0) + (w) * (2 * (i) + 1) / (2 * (n)))
		int pat = 0;
		sidecar_seqed_debug (SEQED_KEY, 0, 2, top, -1, -1);
		run (3);
		if (o) tap (KEYAT (SLX (194), SLX (292) - SLX (194), 2, 3), y2); else tap (KEYAT (LU (8) + LU (40), (LU (684) + DX) * 240 / 684 - LU (40), 2, 3), y2);	// QNT PLAY
		EXPECT_CMD ("seq=key:C:MIN:play"); seqdemo_x (8, &pat); EXPECT ((pat >> 8 & 3) == 2 && (pat >> 4 & 15) == 2);
		tap (o ? SLX (296) + (lw - SLX (296)) / 2 : SLX (366) + 110 * lw / 684 / 2, y2);	// FREE -> fixed
		EXPECT_CMD ("seq=seed:0"); seqdemo_x (8, &pat); EXPECT (pat >> 10 & 1);
		if (!o) { tap (SLX (482) + 60 * lw / 684 / 2, y2); EXPECT_CMD ("seq=seed:1"); EXPECT (seqdemo_seed () == 1); }	// +
		shot ("seq-key-qnt");
		sidecar_seqed_debug (SEQED_PLAY, 0, 2, top, -1, -1);
		run (3);
		tap (o ? KEYAT (0, lw, 1, 10) : KEYAT (LU (8) + LU (4), 340 * lw / 684, 1, 10), y1);			// SPD 1/4
		EXPECT_CMD ("seq=spd:9:1/4"); EXPECT ((seqdemo_x (8, 0) & 15) == 2);
		tap (o ? KEYAT (0, 212 * lw / 352, 2, 6) : KEYAT (SLX (352), lw - LU (4) - 352 * lw / 684, 2, 6), y1 + (o ? y2 - y1 : 0));	// DIR PEND
		EXPECT_CMD ("seq=dir:9:pend"); EXPECT ((seqdemo_x (8, 0) >> 4 & 15) == 2);
		tap (o ? KEYAT (SLX (216), lw - SLX (216), 2, 3) : KEYAT (LU (8) + LU (4), 200 * lw / 684, 2, 3), y2);	// RND TRACK
		EXPECT_CMD ("seq=rnd:9:mode=track,oct=2,wght=flat"); EXPECT ((seqdemo_x (8, 0) >> 8) == 2);
		shot ("seq-play");
		sidecar_seqed_debug (SEQED_STEP, 0, 2, top, 13, 74);
		run (3);
		int fx = 0, lc = 0, rn = 0;
		tap (o ? KEYAT (0, lw, 3, 8) : KEYAT (LU (8) + LU (4), 380 * lw / 684, 3, 8), y1);			// RTRG 4
		EXPECT_CMD ("seq=rtrg:9:14:cnt=4,mode=chord,rate=auto"); EXPECT ((seqdemo_xstep (8, 13, 0, 0, 0) & 255) == 4);
		tap (o ? KEYAT (0, 176 * lw / 352, 1, 6) : KEYAT (SLX (392), lw - LU (4) - 392 * lw / 684, 1, 6), o ? y2 : y1);	// FX OCT+
		EXPECT_CMD ("seq=fx:9:14:1:OCT:1"); seqdemo_xstep (8, 13, &fx, 0, 0); EXPECT ((fx & 255) == 7 && (fx >> 8) == 1);
		tap (o ? KEYAT (SLX (180), SLX (296) - SLX (180), 1, 4) : KEYAT (SLX (192), 260 * lw / 684, 1, 4), y2);	// LOCKS 50%
		EXPECT_CMD ("seq=lcond:9:14:all:50%"); seqdemo_xstep (8, 13, 0, &lc, 0); EXPECT ((lc & 255) == 93);
		tap (o ? SLX (300) + (lw - SLX (300)) / 2 : LU (8) + LU (4) + 120 * lw / 684 / 2, y2);			// RND
		EXPECT_CMD ("seq=rndstep:9:14:1"); seqdemo_xstep (8, 13, 0, 0, &rn); EXPECT (rn == 1);
		run (3);
		shot ("seq-step");
		sidecar_seqed_debug (SEQED_STEP, 0, 2, top, -1, -1);
		run (2);
		shot ("seq-step-none");
		sidecar_seqed_debug (SEQED_ARP, 0, 2, top, 13, 74);
		run (3);
		tap (o ? KEYAT (0, 196 * lw / 352, 2, 7) : KEYAT (LU (8) + LU (4), 400 * lw / 684, 2, 7), y1);		// MODE UP
		EXPECT_CMD ("seq=alock:9:14:0:1"); EXPECT (seqdemo_arpl (8, 13, 0) == 1);
		tap (o ? KEYAT (0, 176 * lw / 352, 6, 7) : KEYAT (LU (8) + LU (4), 400 * lw / 684, 6, 7), y2);		// SPD 1/32
		EXPECT_CMD ("seq=alock:9:14:1:5"); EXPECT (seqdemo_arpl (8, 13, 1) == 5);
		tap (o ? KEYAT (SLX (200), lw - SLX (200), 3, 4) : KEYAT (SLX (412), lw - LU (4) - 412 * lw / 684, 3, 4), y1);	// GATE 100
		EXPECT_CMD ("seq=alock:9:14:3:2"); EXPECT (seqdemo_arpl (8, 13, 3) == 2);
		tap (o ? KEYAT (SLX (180), SLX (262) - SLX (180), 2, 4) : KEYAT (SLX (412), 130 * lw / 684, 2, 4), y2);	// RNG 2
		EXPECT_CMD ("seq=alock:9:14:2:1"); EXPECT (seqdemo_arpl (8, 13, 2) == 1);
		tap (o ? KEYAT (SLX (266), lw - SLX (266), 3, 4) : KEYAT (SLX (548), lw - LU (4) - 548 * lw / 684, 3, 4), y2);	// SWNG 66
		EXPECT_CMD ("seq=alock:9:14:4:2"); EXPECT (seqdemo_arpl (8, 13, 4) == 2);
		tap (o ? KEYAT (0, 176 * lw / 352, 0, 7) : KEYAT (LU (8) + LU (4), 400 * lw / 684, 0, 7), y2);		// SPD's first key: no lock
		EXPECT_CMD ("seq=alock:9:14:1:-1"); EXPECT (seqdemo_arpl (8, 13, 1) == -1);
		run (3);
		shot ("seq-arp");
		sidecar_seqed_debug (SEQED_ARP, 0, 2, top, -1, -1);
		run (2);
		shot ("seq-arp-none");
		for (int f = 0; f < 5; f++)
		{
			char l[40];
			snprintf (l, sizeof l, "seq=alock:9:14:%d:-1", f);
			seqdemo_command (l);
		}
		// (back as it was for the shots after)
		seqdemo_command ("seq=spd:9:follow"), seqdemo_command ("seq=dir:9:fwd"), seqdemo_command ("seq=rnd:9:mode=off");
		seqdemo_command ("seq=rtrg:9:14:off"), seqdemo_command ("seq=fx:9:14:1:off"), seqdemo_command ("seq=lcond:9:14:all:---");
		seqdemo_command ("seq=rndstep:9:14:0"), seqdemo_command ("seq=key:off"), seqdemo_command ("seq=seed:free");
		seqdemo_view (&s_View, s_SeqPlay); sidecar_seqed_view (&s_View);
		sidecar_seqed_debug_key (0, 2, 7, 0, 2);
#undef KEYAT
#undef SLX
	}
	// the velocity lane, all 4 pages: the selected note's bar dragged down to about 40
	sidecar_seqed_playhead (41300);
	sidecar_seqed_debug (SEQED_DRAW, 0, 4, top, 28, 72);
	run (5);
	int mv = 0, vv = 0;
	EXPECT (seqdemo_note (8, 28, 72, &vv, &mv) > 0);
	int lx = gx + (28 * 96 + mv) * gw / (1536 * 4) + (o ? 0 : 1);
	int ltop = o ? LU (4) : LU (8), lhh = lh - (o ? LU (7) : LU (14));
	drag (lx, ly + ltop + lhh - vv * lhh / 127, lx, ly + ltop + lhh - 40 * lhh / 127, 6, "seq-velocity");
	EXPECT (seqdemo_note (8, 28, 72, &vv, 0) > 0 && vv >= 34 && vv <= 46);
	// bigger rows (a pinch): note names in the bars
	sidecar_seqed_debug (SEQED_DRAW, 0, 1, o ? 75 : 76, -1, -1);
	sidecar_seqed_debug_rowh (o ? 12 : 20);
	sidecar_seqed_playhead (9000);
	shot ("seq-zoom");
	sidecar_seqed_debug_rowh (o ? 5 : 14);
	// rui1051 (the hardware report: a 2-page poly track shown as one page while stopped, page 2 only after PLAY): stopped,
	// 32 steps (2 pages: core/seq eff_len now knows them before any tick), one page in the window; the Rytm's PAGE to
	// page 2 (edit_page 1): the window holds page 2, its steps 17..32
	{
		sidecar_seqed_playhead (-1);
		seqdemo_view (&s_View, 0);
		s_View.length = 32, s_View.npages = 2, s_View.edit_page = 0;
		sidecar_seqed_view (&s_View);
		sidecar_seqed_debug (SEQED_DRAW, 0, 1, top, -1, -1);
		run (2);
		EXPECT (sidecar_seqed_first_page () == 0);
		s_View.edit_page = 1;
		sidecar_seqed_view (&s_View);
		run (2);
		EXPECT (sidecar_seqed_first_page () == 1);
		shot ("seq-stopped-page2");
		seqdemo_view (&s_View, s_SeqPlay);
		sidecar_seqed_view (&s_View);
		sidecar_seqed_debug (SEQED_DRAW, 0, 2, top, -1, -1);
	}
	// the hardware report (0.3.0): stopped, a window of 2 or 4 pages showed its notes but the grid's lines only behind
	// the first page. seq-grid-{stopped,playing}-{2,4}pg; seq-grid-stopped-short: stopped, the track 16 steps while the
	// window kept 4 pages from playing (its notes past the end still shown) - past the end shaded, its lines still drawn.
	// Checked: the last visible page's page line differs from the row beside it on most of the grid's height.
	{
		static const struct { int play, pages, len; const char *name; } k[] = {
			{0, 2, 64, "seq-grid-stopped-2pg"}, {0, 4, 64, "seq-grid-stopped-4pg"},
			{1, 2, 64, "seq-grid-playing-2pg"}, {1, 4, 64, "seq-grid-playing-4pg"}, {0, 4, 16, "seq-grid-stopped-short"} };
		int sp0 = s_SeqPlay;
		for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
		{
			s_SeqPlay = k[i].play;
			seqdemo_view (&s_View, k[i].play);
			s_View.length = k[i].len, s_View.npages = (k[i].len + 15) / 16, s_View.edit_page = -1;
			sidecar_seqed_view (&s_View);
			sidecar_seqed_debug (SEQED_DRAW, 0, k[i].pages, top, -1, -1);
			sidecar_seqed_playhead (k[i].play ? 3000 : -1);
			shot (k[i].name);
			int xl = gx + (k[i].pages - 1) * gw / k[i].pages, diff = 0;
			for (int y = gy + 1; y < gy + gh - 1; y++)
				diff += s_Fb[sy (y) * W + sx (xl)] != s_Fb[sy (y) * W + sx (xl + 2)];
			if (diff <= (gh - 2) / 2) printf ("FAIL %s: the last page's line on %d of %d rows\n", k[i].name, diff, gh - 2);
			EXPECT (diff > (gh - 2) / 2);
		}
		s_SeqPlay = sp0;
		seqdemo_view (&s_View, s_SeqPlay);
		sidecar_seqed_view (&s_View);
		sidecar_seqed_playhead (-1);
		sidecar_seqed_debug (SEQED_DRAW, 0, 2, top, -1, -1);
	}

	// ---- the Rytm's own pattern, read only (rytm_fake.c through the 13101-byte record and back)
	static rytm_pattern_t rp, rd;
	static unsigned char rec[RYTM_PAT_BYTES];
	rytm_fake (&rp);
	rytm_pattern_encode (&rp, rec);
	EXPECT (rytm_pattern_decode (rec, sizeof rec, &rd) == 0);
	sidecar_rytmpat_set (&rd, 3, "SHARED SET", "KIT 07  BROKEN BEAT", "sample pattern (host render)");
	sidecar_rytmpat_page (0);
	const int prh = (o ? LU (10) : LU (22)) + DY * 2 / 3 / 12, pcw = (o ? LU (256) + DX * 4 / 7 : LU (640) + DX * 6 / 7) / 64;	// sidecar_rytmpat.c
	if (o) { tap_raw (NAV_X, NAV_Y (2)); tap (LU (10), LU (48) + LU (8) * prh + prh / 2); }		// PAT, the CH row
	else { tap (LU (720) + DX, LU (52) + LU (30)); tap (LU (8) + LU (20), LU (52) + LU (70) + LU (8) * prh + prh / 2); }
	sidecar_rytmpat_select (8, 3);
	sidecar_rytmpat_playhead (13);
	shot ("rytm-pattern");
	if (o) { tap (LU (176) + 9 * DX / 20, LU (26)); tap (LU (24) + LU (60) * pcw + 2, LU (48) + prh + prh / 2); }	// ALL, SD step 61
	else { tap (LU (404) + DX + 4 * LU (52) + LU (26), LU (52) + LU (28)); tap (LU (52) + LU (60) * pcw + LU (5), LU (52) + LU (70) + 1 * prh + prh / 2); }
	sidecar_rytmpat_playhead (58);
	shot ("rytm-all");
	sidecar_rytmpat_page (0);
	// the Pi's path (pi/engine/rytmpat_pi.cpp): the Rytm's answer to the dump request read by core/seq/rytm_dump, then
	// decoded; the page's fetch states (kernel.cpp's texts)
	{
		static unsigned char msg[16384], rec2[RYTM_PAT_BYTES];
		static rytm_pattern_t dp;
		rp.t[0].trig[14] |= RYTM_TRIG_ON;		// the record as fetched differs from the sample: a trig added on BD
		rp.t[0].vel[14] = 127;
		rytm_pattern_encode (&rp, rec);
		int n = rytm_dump_of (rec, 21, msg, sizeof msg), pat = -1;
		EXPECT (n == 14988);
		EXPECT (seqdemo_rytm_parse (msg, n, rec2, &pat) == 1 && pat == 21 && !memcmp (rec, rec2, sizeof rec));
		msg[200] ^= 1;
		EXPECT (seqdemo_rytm_parse (msg, n, rec2, &pat) == 0);	// a damaged answer refused
		EXPECT (rytm_pattern_decode (rec, sizeof rec, &dp) == 0 && (dp.t[0].trig[14] & RYTM_TRIG_ON));
		sidecar_rytmpat_set (&dp, pat, "SHARED SET", "", "from the Rytm");
		sidecar_rytmpat_select (0, 14);
		sidecar_rytmpat_playhead (-1);
		shot ("rytm-dump");
		sidecar_rytmpat_where ("fetching...", 1);
		shot ("rytm-fetching");
		sidecar_rytmpat_where ("no answer, 12 s old", 1);
		shot ("rytm-stale");
		sidecar_rytmpat_where ("from the Rytm", 0);
	}

	// ---- cost: the Mac's microseconds; x 34.1 = a Pi 4 estimate (the old CLASSIC theme's home page: 217 us here, 7.4 ms
	// measured on the Pi 4)
	s_bQuiet = 1;
	sidecar_ui_show (0); run (3);
	double home = bench (0, 50);
	sidecar_ui_show (4); run (3);
	sidecar_seqed_debug (SEQED_DRAW, 0, 2, top, -1, -1);
	sidecar_seqed_playhead (13400);
	run (3);
	double seq = bench (0, 50);
	sidecar_seqed_geom (&gx, &gy, &gw, &gh, &rh, &ly, &lh);
	lv_area_t grid = {gx, gy, gx + gw - 1, gy + gh - 1};
	lv_area_t note = {gx + gw / 3, gy + gh / 3, gx + gw / 3 + gw / 8, gy + gh / 3 + rh * 2};
	lv_area_t head = {gx + gw / 2, gy - 14 / sidecar_ui_dot (), gx + gw / 2 + 4 / sidecar_ui_dot (), gy + gh - 1};
	double gr = bench (&grid, 50), nt = bench (&note, 200), hd = bench (&head, 500);
	// the play head as it runs: a step's movement with the notes lighting under it
	double t0 = now_us ();
	for (int i = 0; i < 64; i++) { sidecar_seqed_playhead (13400 + i * 250); lv_refr_now (NULL); }
	double play = (now_us () - t0) / 64;
	sidecar_ui_show (5); run (3);
	double ry = bench (0, 50);
	sidecar_ui_show (7); wg_fake (WGV_GRANULAR, 0.5); sidecar_wgview_set (&s_Wg); run (3);
	double t1 = now_us ();
	for (int i = 0; i < 50; i++) { wg_fake (WGV_GRANULAR, 0.5 + i / 15.0); sidecar_wgview_set (&s_Wg); lv_refr_now (NULL); }
	double wgg = (now_us () - t1) / 50;
	t1 = now_us ();
	for (int i = 0; i < 50; i++) { wg_fake (WGV_WAVETABLE, 0.5 + i / 15.0); sidecar_wgview_set (&s_Wg); lv_refr_now (NULL); }
	double wgt = (now_us () - t1) / 50;
	printf ("cost %s machine view frame: GRANULAR %.0f us (Pi 4 est. %.1f ms), WAVETABLE %.0f us (%.1f ms)\n", s_Pre, wgg,
		wgg * 7400.0 / 217.0 / 1000, wgt, wgt * 7400.0 / 217.0 / 1000);
	// the REC page: a full draw, and a meter frame (a view with every level moving: the cells' strips that changed)
	sidecar_ui_show (11); rec_play (SRV_REC, 61, 3); run (3);
	double rcf = bench (0, 50);
	t1 = now_us ();
	for (int i = 0; i < 50; i++) { rec_fake (SRV_REC, 61, 3 + i); sidecar_rec_set (&s_Rv); lv_tick_inc (40); lv_refr_now (NULL); }
	double rcm = (now_us () - t1) / 50;
	printf ("cost %s REC page: full %.0f us (Pi 4 est. %.1f ms), a meter frame (all 23 moving) %.0f us (%.1f ms)\n", s_Pre, rcf,
		rcf * 7400.0 / 217.0 / 1000, rcm, rcm * 7400.0 / 217.0 / 1000);
	// touch2: SCOPE - a full draw, a frame (a new trace and the meters), a meters-only frame (frozen); STATUS - a full
	// draw and a second's update (a few numbers changed)
	sidecar_ui_show (12); sidecar_scope_debug (1, 1, 0, 0); scope_fake (30); sidecar_scope_set (&s_Sc); run (3);
	double scf = bench (0, 50);
	t1 = now_us ();
	for (int i = 0; i < 50; i++) { scope_fake (31 + i); sidecar_scope_set (&s_Sc); lv_tick_inc (66); lv_refr_now (NULL); }
	double sct = (now_us () - t1) / 50;
	sidecar_scope_debug (1, 1, 0, 1);
	lv_refr_now (NULL);
	t1 = now_us ();
	for (int i = 0; i < 50; i++) { scope_fake (81 + i); sidecar_scope_set (&s_Sc); lv_tick_inc (66); lv_refr_now (NULL); }
	double scm = (now_us () - t1) / 50;
	sidecar_scope_debug (-1, 1, 0, 0);
	sidecar_ui_show (13); status_fake (0); sidecar_status_set (&s_Stv); run (3);
	double stf = bench (0, 50);
	t1 = now_us ();
	for (int i = 0; i < 50; i++)
	{
		status_fake (0); s_Stv.load[1] = 30 + i % 20; s_Stv.vload[2] = 60 + i; s_Stv.block_max_us = 150 + i; s_Stv.gen = 9000 + i;
		sidecar_status_set (&s_Stv); lv_refr_now (NULL);
	}
	double stu = (now_us () - t1) / 50;
	printf ("cost %s SCOPE: full %.0f us (Pi 4 est. %.1f ms), a frame (trace + meters) %.0f us (%.1f ms), frozen (meters) %.0f us (%.2f ms) | "
		"STATUS: full %.0f us (%.1f ms), a second's update %.0f us (%.2f ms)\n", s_Pre, scf, scf * 7400.0 / 217.0 / 1000,
		sct, sct * 7400.0 / 217.0 / 1000, scm, scm * 7400.0 / 217.0 / 1000, stf, stf * 7400.0 / 217.0 / 1000, stu, stu * 7400.0 / 217.0 / 1000);
	// a theme switch: the rebuild and its first full draw (and the remote app's unfed page: its panel and greyed key survive the rebuild)
	sidecar_ui_show (4); sidecar_ui_page_enable (4, 0); run (3);
	t0 = now_us ();
	sidecar_ui_style (o ? THEME_SIDECAR : THEME_OLED);
	double swb = now_us () - t0, band = 0;
	for (int i = 0; i < 8; i++)				// the wipe: a band a call (the longest one counts)
	{
		lv_tick_inc (40);
		double b0 = now_us ();
		lv_timer_handler ();
		if (now_us () - b0 > band) band = now_us () - b0;
		if (getenv ("BANDS")) printf ("  band %d: %.0f us\n", i, now_us () - b0);
	}
	double sw = now_us () - t0;
	EXPECT (sidecar_ui_nofeed_shown () && !sidecar_ui_nav_enabled (1) && sidecar_ui_nav_enabled (2));
	sidecar_ui_style (style);
	run (3);
	EXPECT (sidecar_ui_nofeed_shown () && !sidecar_ui_nav_enabled (1) && sidecar_ui_nav_enabled (2));
	sidecar_ui_page_enable (4, 1); sidecar_ui_show (0); run (3);
	EXPECT (!sidecar_ui_nofeed_shown () && sidecar_ui_nav_enabled (1));
	const double k = 7400.0 / 217.0;
	printf ("cost %s (Mac us -> Pi 4 est. ms, x %.1f): home %.0f (%.1f) | SEQ full %.0f (%.1f), grid %.0f (%.1f), note drag frame %.0f (%.2f), "
		"play head strip %.1f (%.2f), playing frame %.1f (%.2f) | RYTM full %.0f (%.1f) | switch to %s %.0f (%.1f): the rebuild %.0f (%.1f), the longest band %.0f (%.1f)\n",
		s_Pre, k, home, home * k / 1000, seq, seq * k / 1000, gr, gr * k / 1000, nt, nt * k / 1000, hd, hd * k / 1000,
		play, play * k / 1000, ry, ry * k / 1000, o ? "SIDECAR" : "OLED", sw, sw * k / 1000, swb, swb * k / 1000, band, band * k / 1000);
	s_bQuiet = 0;
}

// 0.5.1: the high contrast (SIDECAR): the Display page's Contrast row sends uicontrast= and rebuilds in that look; a stored
// value (status ui_contrast) does the same. After the theme runs: a rebuild would reset what their gestures set up.
static void contrast_run (void)
{
	snprintf (s_Pre, sizeof s_Pre, "sidecar");
	s_nContrastSt = -1;
	status_init (THEME_SIDECAR);
	sidecar_ui_status (&s_St);
	run (12);
	DX = TH->dx;
	const int cy = LU (110) + 266 * (LU (358) + DY) / 358 + (LU (40) + DY / 15) / 2;
	sidecar_ui_show (6); run (3);
	tap (LU (12) + LU (190) + (LU (570) + DX) * 3 / 8, cy); EXPECT_CMD ("uicontrast=blue"); run (3);
	EXPECT (sidecar_theme_get_contrast () == 1 && TH->hc == 1 && TH->edge_w == 3 && TH->id == THEME_SIDECAR);
	sidecar_ui_show (6); run (2); shot ("hc-display-tap");
	tap (LU (12) + LU (190) + (LU (570) + DX) * 5 / 8, cy); EXPECT_CMD ("uicontrast=green"); run (3);
	EXPECT (sidecar_theme_get_contrast () == 2 && TH->hc == 2);
	tap (LU (12) + LU (190) + (LU (570) + DX) * 7 / 8, cy); EXPECT_CMD ("uicontrast=viper"); run (3);
	EXPECT (sidecar_theme_get_contrast () == 3 && TH->hc == 3 && TH->edge_w == 2 && TH->id == THEME_SIDECAR);
	contrast_check ();
	tap (LU (12) + LU (190) + (LU (570) + DX) / 8, cy); EXPECT_CMD ("uicontrast=off"); run (3);
	EXPECT (sidecar_theme_get_contrast () == 0 && TH->hc == 0 && TH->edge_w == 2);
	s_St.ui_contrast = 2; sidecar_ui_status (&s_St); run (3);		// a stored value arrives
	EXPECT (TH->hc == 2);
	s_St.ui_contrast = 0; sidecar_ui_status (&s_St); run (3);
	EXPECT (TH->hc == 0);
	// the OLED theme keeps a stored value and ignores it
	sidecar_ui_style (THEME_OLED); run (12);
	s_St.ui_style = THEME_OLED; s_St.ui_contrast = 1; sidecar_ui_status (&s_St); run (3);
	EXPECT (sidecar_theme_get_contrast () == 1 && TH->id == THEME_OLED && TH->hc == 0 && TH->edge_w == 1);
	s_St.ui_contrast = 0; sidecar_ui_status (&s_St); run (3);
	sidecar_ui_style (THEME_SIDECAR); run (12);
	s_St.ui_style = THEME_SIDECAR;
}

// WCAG contrast of two 0xRRGGBB colours (the web app's tokens.css check, as the theme's contrast targets say)
static double lum (uint32_t c)
{
	double v[3];
	for (int i = 0; i < 3; i++)
	{
		double x = (double) (c >> (16 - 8 * i) & 255) / 255.0;
		v[i] = x <= 0.03928 ? x / 12.92 : pow ((x + 0.055) / 1.055, 2.4);
	}
	return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}
static double ratio (uint32_t a, uint32_t b)
{
	double x = lum (a), y = lum (b);
	return (x > y ? x + 0.05 : y + 0.05) / (x > y ? y + 0.05 : x + 0.05);
}
// 0.5.1: the high-contrast tables (the table now: blue or green): text >= 4.5:1, controls and lines >= 3:1 on the black
#define CONTRAST_AT_LEAST(a, b, min) do { double r_ = ratio (a, b); if (r_ < (min)) { printf ("FAIL shot.c:%d [%s] contrast %s on %s is %.2f:1, wanted %.1f:1\n", __LINE__, s_Pre, #a, #b, r_, (double) (min)); s_nFail++; } } while (0)
static void contrast_check (void)
{
	const uint32_t bg = TH->bg;
	CONTRAST_AT_LEAST (TH->ink, bg, 4.5); CONTRAST_AT_LEAST (TH->dim, bg, 4.5); CONTRAST_AT_LEAST (TH->faint, bg, 4.5);
	CONTRAST_AT_LEAST (TH->sub, bg, 4.5); CONTRAST_AT_LEAST (TH->win_ink, TH->win, 4.5); CONTRAST_AT_LEAST (TH->win_dim, TH->win, 4.5);
	CONTRAST_AT_LEAST (TH->accent, bg, 4.5); CONTRAST_AT_LEAST (TH->bad, bg, 4.5); CONTRAST_AT_LEAST (TH->warn, bg, 4.5);
	CONTRAST_AT_LEAST (TH->lock, bg, 4.5); CONTRAST_AT_LEAST (TH->play, bg, 4.5);
	CONTRAST_AT_LEAST (TH->sel_ink, TH->sel_bg, 4.5);			// the selected key: black on the accent
	CONTRAST_AT_LEAST (TH->win_edge, TH->win, 3.0);			// a card's and a control's edge
	CONTRAST_AT_LEAST (TH->line_beat, TH->row_in, 3.0); CONTRAST_AT_LEAST (TH->line_page, TH->row_in, 3.0);
	CONTRAST_AT_LEAST (TH->hc == 3 ? TH->faint : 0x767676, bg, 3.0);	// (Viper: its dim green; its graticule tracks are decor) the empty track of a knob / meter (sidecar_theme.c UV.trk)
	CONTRAST_AT_LEAST (TH->key_white, TH->key_black, 3.0);
	CONTRAST_AT_LEAST (TH->accent, TH->row_root, 4.5);			// a note on the root rows
	CONTRAST_AT_LEAST (0x000000, TH->accent, 4.5); CONTRAST_AT_LEAST (0x000000, TH->bad, 4.5);	// a note's text, the selected note's
}

// ---- the animation: the OLED SEQ page playing the demo pattern (strummed chords, a humanised melody), a note dragged,
// a chord stamped, the velocity lane touched, 20 frames a second
static int s_Frame, s_Frames;
static void frame (const char *dir, int *pos, int dpos)
{
	char p[400];
	if (s_Frame >= s_Frames) return;
	*pos += dpos;
	sidecar_seqed_playhead (*pos % (64 * 1000));
	lv_tick_inc (50);
	lv_timer_handler ();
	lv_refr_now (NULL);
	snprintf (p, sizeof p, "%s/frame-%04d.png", dir, s_Frame++);
	write_png (p);
}

static void frames (const char *dir, int *pos, int dpos, int n) { for (int i = 0; i < n; i++) frame (dir, pos, dpos); }

static void anim (const char *dir, int nframes)
{
	s_Frames = nframes;
	s_bQuiet = 1;
	status_init (THEME_OLED);
	sidecar_ui_status (&s_St);
	seqdemo_build ();
	seqdemo_view (&s_View, 1);
	sidecar_seqed_view (&s_View);
	sidecar_ui_show (4);
	sidecar_seqed_debug (SEQED_DRAW, 0, 2, 77, -1, -1);
	run (3);
	int gx, gy, gw, gh, rh, ly, lh;
	sidecar_seqed_geom (&gx, &gy, &gw, &gh, &rh, &ly, &lh);
	const int dpos = LU (405);			// 121.5 BPM: a step is 123.5 ms, a 50 ms frame 0.405 of a step
	int pos = 0;
	frames (dir, &pos, dpos, 50);		// 0 .. 2.5 s: playing page 1-2, the strummed chords lighting up
	// 2.5 .. 5 s: the melody's note on step 7 dragged two steps later and a tone up, its card along
	int m = 0, vel = 0, st = 6, pitch = 75;
	if (!seqdemo_note (8, st, pitch, &vel, &m)) { st = 2; pitch = 72; seqdemo_note (8, st, pitch, &vel, &m); }
	int nx = gx + (st * 96 + m) * gw / (1536 * 2) + 2, ny = gy + (77 - pitch) * rh + 1;
	int x1 = nx + 2 * gw / 32, y1 = ny - 2 * rh;
	s_Px = sx (nx); s_Py = sy (ny); s_Down = 1;
	frames (dir, &pos, dpos, 6);
	for (int i = 1; i <= 24; i++)
	{
		s_Px = sx (nx + (x1 - nx) * i / 24); s_Py = sy (ny + (y1 - ny) * i / 24);
		frame (dir, &pos, dpos);
	}
	frames (dir, &pos, dpos, 6);
	s_Down = 0;
	frames (dir, &pos, dpos, 14);
	// 5 .. 9 s: the CHORD tool, MAJ7 broken by 12/96 a note, stamped on step 9 of page 2 (C4)
	s_Px = sx (73); s_Py = sy (229); s_Down = 1; frames (dir, &pos, dpos, 3); s_Down = 0; frames (dir, &pos, dpos, 8);
	int cw = (352 + 3) / 11;
	s_Px = sx (3 * cw + cw / 2); s_Py = sy (171 + 10); s_Down = 1; frames (dir, &pos, dpos, 3); s_Down = 0; frames (dir, &pos, dpos, 8);
	s_Px = sx (192 + 2 * 41 + 20); s_Py = sy (171 + 33); s_Down = 1; frames (dir, &pos, dpos, 3); s_Down = 0; frames (dir, &pos, dpos, 6);
	int cx = gx + 24 * gw / 32 + gw / 64, cy = gy + (77 - 60) * rh + 1;
	s_Px = sx (cx); s_Py = sy (cy); s_Down = 1; frames (dir, &pos, dpos, 3); s_Down = 0;
	frames (dir, &pos, dpos, 46);
	// 9 .. 11.5 s: back to DRAW; the new chord's top note's velocity pulled down in the lane
	s_Px = sx (24); s_Py = sy (229); s_Down = 1; frames (dir, &pos, dpos, 3); s_Down = 0; frames (dir, &pos, dpos, 6);
	int lx = gx + (24 * 96 + 36) * gw / 3072 + 1, lt = ly + LU (4), lhh = lh - LU (7);
	s_Px = sx (lx); s_Py = sy (lt + 4); s_Down = 1;
	for (int i = 1; i <= 20; i++) { s_Py = sy (lt + 4 + (lhh - 20) * i / 20); frame (dir, &pos, dpos); }
	s_Down = 0;
	while (s_Frame < s_Frames) frame (dir, &pos, dpos);	// the rest: playing on, the window following
	printf ("anim: %d frames in %s\n", s_Frame, dir);
}

// ./shot conn OUTDIR [WxH]: the link animation (sidecar_ui.c conn_*) in both themes: the Rytm comes up (frames at the
// listed ms), then drops; <theme>-up-<ms>.png, <theme>-down-<ms>.png; the cost of a frame on the Mac is printed
static void conn_run (const char *dir)
{
	static const int kUp[] = {0, 120, 260, 480, 700, 1000, 1200, 1380, 1500}, kDown[] = {0, 140, 400, 800, 1000, 1120};
	char p[400];
	s_bQuiet = 1;
	for (int st = 0; st < 2; st++)
	{
		const char *pre = st ? "sidecar" : "oled";
		sidecar_ui_style (st ? THEME_SIDECAR : THEME_OLED);
		run (12);
		status_init (st ? THEME_SIDECAR : THEME_OLED);
		s_St.rytm = 0;
		sidecar_ui_status (&s_St);
		sidecar_ui_show (0);
		run (8);
		for (int dir_up = 1; dir_up >= 0; dir_up--)
		{
			const int *k = dir_up ? kUp : kDown, nk = dir_up ? (int) (sizeof kUp / sizeof *kUp) : (int) (sizeof kDown / sizeof *kDown);
			s_St.rytm = dir_up;
			sidecar_ui_status (&s_St);
			double worst = 0, sum = 0;
			int frames = 0;
			for (int ms = 0, i = 0; i < nk; ms += 20)
			{
				double t0 = now_us ();
				lv_timer_handler ();
				lv_refr_now (NULL);
				double us = now_us () - t0;
				if (ms) { if (us > worst) worst = us; sum += us; frames++; }
				if (ms >= k[i])
				{
					snprintf (p, sizeof p, "%s/%s-%s-%04d.png", dir, pre, dir_up ? "up" : "down", k[i]);
					write_png (p);
					i++;
				}
				lv_tick_inc (20);
			}
			printf ("conn %s %s: %d frames, mean %.0f us, worst %.0f us (Mac, flush included)\n", pre, dir_up ? "up" : "down",
				frames, sum / frames, worst);
			run (10);
		}
		s_St.rytm = 1;						// a page change and a repeated status must not start it again
		sidecar_ui_status (&s_St);
		sidecar_ui_show (1);
		sidecar_ui_show (0);
		sidecar_ui_status (&s_St);
	}
}

// ./shot a4 OUTDIR [WxH]: an A4 the box in use (core/device/a4.cpp: "A4", voices T1..T4, "-" the four it does not use): the connect
// splash, HOME (connected and not), STATUS, REC, SCOPE, INSERT FX and Sound in both themes say the A4 and T1..T4, never the Rytm's
// tracks; a4-<theme>-<page>.png. The Rytm's own shots stay the plain run's.
static void a4_run (const char *dir)
{
	static const char *const kT[8] = {"T1", "T2", "T3", "T4", "-", "-", "-", "-"};
	snprintf (s_Dir, sizeof s_Dir, "%s", dir);
	char p[400];
	for (int st = 0; st < 2; st++)
	{
		const char *th = st ? "sidecar" : "oled";
		snprintf (s_Pre, sizeof s_Pre, "a4-%s", th);
		sidecar_ui_style (st ? THEME_SIDECAR : THEME_OLED);
		run (12);
		status_init (st ? THEME_SIDECAR : THEME_OLED);
		DX = TH->dx;
		s_St.box = "A4";
		for (int v = 0; v < 8; v++) s_St.voice[v] = kT[v];
		s_St.rytm = 0;
		sidecar_ui_status (&s_St);			// a new box: the pages rebuilt with its words
		sidecar_ui_show (0);
		run (8);
		EXPECT (!strcmp (sidecar_ui_box (), "A4") && !strcmp (sidecar_ui_box_up (), "A4") && sidecar_ui_box_names ());
		EXPECT (!strcmp (sidecar_ui_voice (1), "T2") && sidecar_ui_voice_at (1) == 1 && sidecar_ui_voice_pos (3) == 3);
		EXPECT (sidecar_ui_get_style () == (st ? THEME_SIDECAR : THEME_OLED));
		s_St.rytm = 1;
		sidecar_ui_status (&s_St);			// the link comes up: the connect splash
		for (int ms = 0; ms <= 700; ms += 20) { lv_timer_handler (); lv_refr_now (NULL); lv_tick_inc (20); }
		snprintf (p, sizeof p, "%s/%s-conn-up.png", dir, s_Pre);
		write_png (p);
		run (60);
		shot ("home");
		sidecar_ui_show (13); status_fake (0); sidecar_status_set (&s_Stv); run (2); shot ("status");
		sidecar_ui_show (11); rec_fake (SRV_ARMED, 0, 0);
		for (int k = 0; k < 8; k++)		// as rec_pi.cpp names them for the A4: its stems and voices
		{
			snprintf (s_Rv.name[3 + k], sizeof s_Rv.name[0], "%s", kT[k]);
			snprintf (s_Rv.name[13 + k], sizeof s_Rv.name[0], "%s", kT[k]);
		}
		sidecar_rec_set (&s_Rv); run (2); shot ("rec");
		sidecar_ui_show (12); sidecar_scope_debug (-1, 1, 0, 0);
		for (int f = 0; f < 6; f++)
		{
			scope_fake (f);
			for (int k = 0; k < 8; k++) snprintf (s_Sc.name[1 + k], sizeof s_Sc.name[0], "%s", kT[k]);	// tap_source_name for the A4
			sidecar_scope_set (&s_Sc); run (1);
		}
		shot ("scope");
		sidecar_ui_show (8); voice_fake (1); s_Vv.track = 3; s_Vv.gen++; sidecar_voice_set (&s_Vv); run (2); shot ("voice");	// the A4's T4
		sidecar_ui_show (9); ins_fake (11); sidecar_insfx_set (&s_Iv); run (2); shot ("insfx");
		sidecar_ui_show (1); run (2); shot ("sound");
		sidecar_ui_show (0); s_St.rytm = 0; sidecar_ui_status (&s_St); run (60); shot ("home-off");
		s_St.box = 0;					// back to the Rytm: its words again
		for (int v = 0; v < 8; v++) s_St.voice[v] = 0;
		s_St.rytm = 1;
		sidecar_ui_status (&s_St); run (60);
		EXPECT (!strcmp (sidecar_ui_box (), "Rytm") && !sidecar_ui_box_names () && !strcmp (sidecar_ui_voice (1), "BT") && sidecar_ui_voice_at (1) == 2);
		shot ("home-rytm-again");
	}
	if (s_nFit) printf ("shot a4: %d strings do not fit (TEXT-FIT above)\n", s_nFit);
	printf (s_nFail ? "shot a4: %d checks FAILED\n" : "shot a4: all checks ok (both themes)\n", s_nFail);
}


// ---- fuzz: ./shot fuzz SEED N [WxH] [hc|hcg|viper|grid|sidecar]: Pi-like touch events at random ----
// Build with the sanitizers (a clean obj/): make shot CXX="clang++ -fsanitize=address,undefined"
//   CFLAGS="-O1 -w -I obj -I \$(LVGL) -DLV_CONF_INCLUDE_SIMPLE -fsanitize=address,undefined -fno-omit-frame-pointer"
// then ./shot fuzz 0 0 1280x720 replay0|1|2 (the PRESET page, every object tapped: the POOL-from-PRESET recursion of
// 0.5.1 overflowed the stack here) and ./shot fuzz 1 20000 1280x720 sidecar (seeds 1 3 5, every look).
static uint64_t s_Rng;
static unsigned rnd (unsigned n) { s_Rng ^= s_Rng << 13; s_Rng ^= s_Rng >> 7; s_Rng ^= s_Rng << 17; return (unsigned) ((s_Rng >> 11) % (n ? n : 1)); }
static long s_nEv;
static char s_Ev[64][96]; static int s_nEvI;
static void evlog (const char *f, int a, int b, int c, int d)
{
	snprintf (s_Ev[s_nEvI & 63], 96, f, a, b, c, d); if (getenv ("FZ_TRACE")) fprintf (stderr, "EV#%ld %s\n", s_nEv, s_Ev[s_nEvI & 63]); s_nEvI++;
}
static void fz_dump (void) { fprintf (stderr, "fuzz: last events (event #%ld):\n", s_nEv); for (int i = s_nEvI > 64 ? s_nEvI - 64 : 0; i < s_nEvI; i++) fprintf (stderr, "  %s\n", s_Ev[i & 63]); }
// the Pi's Update: the touch event reaches sidecar_ui_touch, LVGL runs
static void fz_step (int ms)
{
	sidecar_ui_touch (s_Down, s_Px, s_Py, s_Px, s_Py);
	lv_tick_inc (ms); lv_timer_handler ();
}
static lv_obj_t *s_Cand[4096]; static int s_nCand;
static void fz_walk (lv_obj_t *o, int depth)
{
	if (depth > 12 || s_nCand >= 4096) return;
	if (lv_obj_has_flag (o, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_has_flag (o, LV_OBJ_FLAG_HIDDEN)) s_Cand[s_nCand++] = o;
	uint32_t n = lv_obj_get_child_count (o);
	for (uint32_t i = 0; i < n; i++) fz_walk (lv_obj_get_child (o, (int32_t) i), depth + 1);
}
static void fz_pick (int *x, int *y)		// UI coordinates: an object's middle (random point in it), or anywhere
{
	s_nCand = 0;
	fz_walk (lv_layer_top (), 0); fz_walk (lv_screen_active (), 0);
	if (s_nCand && rnd (10) < 8)
	{
		lv_area_t a; lv_obj_get_coords (s_Cand[rnd (s_nCand)], &a);
		*x = a.x1 + (int) rnd ((unsigned) (a.x2 - a.x1 + 1)); *y = a.y1 + (int) rnd ((unsigned) (a.y2 - a.y1 + 1));
	}
	else { *x = (int) rnd ((unsigned) (W / sidecar_ui_dot ())); *y = (int) rnd ((unsigned) (H / sidecar_ui_dot ())); }
}
static void fz_set (int x, int y) { s_Px = sx (x); s_Py = sy (y); }
static void fz_second (int on)			// the second finger (the Pi's m_nPointer2 -> sidecar_seqed_touch2: screen coordinates)
{
	int x = (int) rnd ((unsigned) W), y = (int) rnd ((unsigned) H);
	sidecar_ui_to_ui (&x, &y);
	sidecar_seqed_touch2 (on, x, y);
}
static void fz_event (void)
{
	int x, y, x1, y1;
	unsigned k = rnd (100);
	s_nEv++;
	if (k < 25)			// tap
	{
		fz_pick (&x, &y); evlog ("tap %d,%d", x, y, 0, 0);
		fz_set (x, y); s_Down = 1; for (unsigned i = rnd (4) + 1; i--; ) fz_step (10 + rnd (30));
		s_Down = 0; for (unsigned i = rnd (3) + 1; i--; ) fz_step (10 + rnd (30));
	}
	else if (k < 40)		// long press (LVGL 400 ms), released at a random time
	{
		fz_pick (&x, &y); int ms = 100 + (int) rnd (1800); evlog ("hold %d,%d %dms", x, y, ms, 0);
		fz_set (x, y); s_Down = 1; for (int t = 0; t < ms; t += 40) fz_step (40);
		s_Down = 0; fz_step (10 + rnd (50));
	}
	else if (k < 65)		// drag, possibly ending anywhere, any speed
	{
		fz_pick (&x, &y); fz_pick (&x1, &y1); int steps = 2 + (int) rnd (30), ms = 5 + (int) rnd (60); evlog ("drag %d,%d > %d,%d", x, y, x1, y1);
		evlog ("  steps %d ms %d", steps, ms, 0, 0);
		fz_set (x, y); s_Down = 1; fz_step (ms);
		for (int i = 1; i <= steps; i++) { fz_set (x + (x1 - x) * i / steps, y + (y1 - y) * i / steps); fz_step (ms); }
		if (rnd (4)) { s_Down = 0; fz_step (ms); }	// 1 in 4: the release comes with the next event (a page change in between)
	}
	else if (k < 75)		// a second finger around a press
	{
		fz_pick (&x, &y); evlog ("two-finger %d,%d", x, y, 0, 0);
		fz_set (x, y); s_Down = 1; fz_step (30); fz_second (1);
		for (int i = rnd (8); i--; ) { fz_second (1); fz_set (x + (int) rnd (60) - 30, y + (int) rnd (60) - 30); fz_step (20); }
		if (rnd (2)) { fz_second (0); s_Down = 0; } else { s_Down = 0; fz_second (0); }
		fz_step (30);
	}
	else if (k < 90)		// a page change, with or without a finger down
	{
		int n = sidecar_ui_page_count (), pg = (int) rnd ((unsigned) n); evlog ("show %d", pg, 0, 0, 0);
		sidecar_ui_show (pg); fz_step (20 + rnd (80));
	}
	else if (k < 95)		// status churn
	{
		evlog ("status", 0, 0, 0, 0);
		s_St.transport = (int) rnd (2); s_St.pattern = (int) rnd (16); s_St.rec = (int) rnd (3); s_St.rec_secs = (int) rnd (900);
		s_St.usb_mode = (int) rnd (2); sidecar_ui_status (&s_St); fz_step (20 + rnd (200));
	}
	else if (k < 97)		// idle
	{
		evlog ("idle", 0, 0, 0, 0); for (int i = rnd (30); i--; ) fz_step (40);
	}
	else				// a theme switch (the whole UI rebuilt), maybe with the finger still down
	{
		static const int st[3] = {THEME_OLED, THEME_SIDECAR, THEME_GRID};
		int t = st[rnd (3)]; evlog ("style %d", t, 0, 0, 0);
		sidecar_ui_style (t); for (int i = 0; i < 10; i++) fz_step (40);
	}
}
static int fuzz_main (int argc, char **argv)
{
	unsigned long seed = strtoul (argv[2], 0, 10); long n = atol (argv[3]);
	s_Rng = seed * 0x9E3779B97F4A7C15ull + 88172645463325252ull;
	if (W != 1280 || H != 720) { fprintf (stderr, "fuzz: 1280x720 only\n"); return 2; }
	s_Dots = (uint16_t *) malloc ((size_t) W * 16 * SIDECAR_DOT * sizeof (uint16_t));	// m_pDotBuf: m_nWidth * DotRows * SIDECAR_DOT
	s_Panel = (uint16_t *) calloc ((size_t) 720 * 1280, sizeof (uint16_t));		// the 720x1280 panel frame buffer
	const char *look = argc > 4 ? argv[4] : "plain";
	int style = !strcmp (look, "grid") ? THEME_GRID : !strcmp (look, "sidecar") || !strcmp (look, "hc") || !strcmp (look, "hcg") || !strcmp (look, "viper") ? THEME_SIDECAR : THEME_OLED;
	s_nContrastSt = !strcmp (look, "hc") ? 1 : !strcmp (look, "hcg") ? 2 : !strcmp (look, "viper") ? 3 : 0;
	s_bQuiet = 1;
	seqdemo_build ();
	status_init (THEME_OLED);
	sidecar_ui_status (&s_St);
	if (style != THEME_OLED) { status_init (style); sidecar_ui_style (style); for (int i = 0; i < 10; i++) fz_step (40); }
	if (!strcmp (look, "replay") || !strncmp (look, "replay", 6))	// ./shot fuzz 0 0 1280x720 replayN: theme N, PRESET page, then every clickable object in turn
	{
		int t = look[6] ? look[6] - '0' : 0; static const int st[3] = {THEME_OLED, THEME_SIDECAR, THEME_GRID};
		status_init (st[t]); sidecar_ui_style (st[t]); for (int i = 0; i < 10; i++) fz_step (40);
		sidecar_ui_show (18); fz_step (50);
		s_nCand = 0; fz_walk (lv_layer_top (), 0); fz_walk (lv_screen_active (), 0);
		for (int c = 0; c < s_nCand; c++)
		{
			lv_area_t a; lv_obj_get_coords (s_Cand[c], &a);
			int x = (a.x1 + a.x2) / 2, y = (a.y1 + a.y2) / 2;
			fprintf (stderr, "REPLAY theme %d: show 18; tap %d,%d (UI coordinates)\n", t, x, y);
			fz_set (x, y); s_Down = 1; fz_step (30); fz_step (30); s_Down = 0; fz_step (30); fz_step (30);
			sidecar_ui_show (18); fz_step (50); s_nCand = 0; fz_walk (lv_layer_top (), 0); fz_walk (lv_screen_active (), 0);
		}
		printf ("replay theme %d: %d objects tapped ok\n", t, s_nCand);
		return 0;
	}
	for (long i = 0; i < n; i++)
	{
		fz_event ();
		if ((i & 1023) == 1023) fprintf (stderr, "fuzz seed %lu %s: %ld events\n", seed, look, i + 1);
	}
	printf ("fuzz seed %lu %s: %ld events ok (blit rejects %ld)\n", seed, look, n, s_nBlitMiss);
	return 0;
}

int main (int argc, char **argv)
{
	lv_init ();
	for (int i = 1; i < argc; i++)				// a screen size anywhere on the line: 1024x600
		if (sscanf (argv[i], "%dx%d", &W, &H) == 2)
		{
			if (W < 320 || H < 240 || W > WMAX || H > HMAX) { fprintf (stderr, "shot: %dx%d: 320x240 .. 1280x720\n", W, H); return 2; }
			for (int k = i; k + 1 < argc; k++) argv[k] = argv[k + 1];
			argc--;
			break;
		}
	lv_display_t *d = lv_display_create (W, H);
	lv_display_set_flush_cb (d, flush);
	lv_display_set_buffers (d, s_Buf, 0, sizeof s_Buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_indev_t *ind = lv_indev_create ();
	lv_indev_set_type (ind, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb (ind, pointer);
	sidecar_ui_create ();
	sidecar_ui_set_command (on_command, 0);
	if (argc > 2 && !strcmp (argv[1], "wganim"))		// ./shot wganim OUTDIR [N] [sidecar]: the machine view, 15 fps
	{
		int n = argc > 3 ? atoi (argv[3]) : 60, cl = argc > 4 && !strcmp (argv[4], "sidecar");
		s_bQuiet = 1;
		status_init (cl ? THEME_SIDECAR : THEME_OLED);
		sidecar_ui_status (&s_St);
		sidecar_ui_show (7);
		char p[400];
		for (int f = 0; f < n; f++)
		{
			wg_fake (f < n / 2 ? WGV_GRANULAR : WGV_WAVETABLE, f / 15.0);
			sidecar_wgview_set (&s_Wg);
			lv_tick_inc (66);
			lv_timer_handler ();
			lv_refr_now (NULL);
			snprintf (p, sizeof p, "%s/frame-%04d.png", argv[2], f);
			write_png (p);
		}
		printf ("wganim: %d frames in %s\n", n, argv[2]);
		return 0;
	}
	if (argc > 2 && !strcmp (argv[1], "a4"))
	{
		th_fit_report = fit_painted;
		a4_run (argv[2]);
		return s_nFail != 0 || s_nFit != 0;
	}
	if (argc > 2 && !strcmp (argv[1], "conn"))
	{
		conn_run (argv[2]);
		return 0;
	}
	if (argc > 2 && !strcmp (argv[1], "anim"))
	{
		anim (argv[2], argc > 3 ? atoi (argv[3]) : 300);
		return 0;
	}
	if (argc > 3 && !strcmp (argv[1], "fuzz")) return fuzz_main (argc, argv);
	snprintf (s_Dir, sizeof s_Dir, "%s", argc > 1 ? argv[1] : "obj");
	th_fit_report = fit_painted;
	theme_run (THEME_OLED, "oled");
	// ./shot DIR [WxH] hc | hcg | viper: the SIDECAR theme's high contrast (0.5.1), blue or green, or Viper: sidecar-hc-*.png / sidecar-hcg-*.png / sidecar-vp-*.png
	// (the OLED run before it is the fresh start the SIDECAR run expects; a run in a process of its own for the same reason)
	const int hc = argc > 2 && !strcmp (argv[2], "hc") ? 1 : argc > 2 && !strcmp (argv[2], "hcg") ? 2 : argc > 2 && !strcmp (argv[2], "viper") ? 3 : 0;
	if (hc) s_nContrastSt = hc;
	// ./shot DIR [WxH] grid: the GRID theme: grid-*.png, the same checks as SIDECAR's
	const int gd = argc > 2 && !strcmp (argv[2], "grid");
	if (gd) theme_run (THEME_GRID, "grid");
	else theme_run (THEME_SIDECAR, hc == 1 ? "sidecar-hc" : hc == 2 ? "sidecar-hcg" : hc == 3 ? "sidecar-vp" : "sidecar");
	theme_run (THEME_OLED, "again");		// back: a second switch rebuilds cleanly (again-*.png)
	if (!hc && !gd) contrast_run ();
	if (s_nFit) printf ("shot: %d strings do not fit (TEXT-FIT above)\n", s_nFit);
	if (s_nFail) printf ("shot: %d checks FAILED\n", s_nFail);
	else printf ("shot: all checks ok (%s)\n", gd ? "OLED, GRID" : hc ? "OLED, SIDECAR high contrast" : "both themes");
	return s_nFail != 0 || s_nFit != 0;
}
