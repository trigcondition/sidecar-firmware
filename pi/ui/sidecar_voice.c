// pi/ui/sidecar_voice.c - see sidecar_voice.h. Eight knobs (th_meter TH_METER_KNOB, the Rytm's labels as captions), a
// code-drawn scope + level (or the machine's panel: 6-OP FM, MULTISAMPLE, DAISYSP), a code-drawn heatmap of the track's
// p-locks; every mark through the theme (th_fill) so the OLED theme keeps its dots: greys, the panel's blue print, the
// FUNC teal (no red); CLASSIC the same drawings at its scale in the display window.
#include "sidecar_voice.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Name, *s_Knob[8], *s_Scope, *s_Mode, *s_Heat, *s_Info, *s_HeatCap;
static sidecar_voice_view_t s_V;
static unsigned s_nGen = ~0u;
static void (*s_Tap) (int what);
static const char *const kTrack[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_bg (void) { return oled () ? TH->bg : TH->win; }

// the level: a bar from the bottom at the area's right, the accent past 3/4; returns the width left of it
static int level_draw (lv_layer_t *l, const lv_area_t *c)
{
	int w = lv_area_get_width (c), h = lv_area_get_height (c), lw = oled () ? 7 : 14;
	int lh = s_V.level * (h - 2) / 127, x0 = w - lw;
	th_fill (l, c, x0, 0, w - 1, h - 1, oled () ? 0x262626 : th_mix (c_bg (), c_dim (), 40), LV_OPA_COVER, 0);
	if (lh > 0) th_fill (l, c, x0, h - 1 - lh, w - 1, h - 1, s_V.level > 96 ? TH->accent : c_ink (), LV_OPA_COVER, 0);
	return w - lw - (oled () ? 4 : 8);
}

static void scope_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int h = lv_area_get_height (&c), sw = level_draw (l, &c);
	int mid = h / 2, amp = h / 2 - 2;
	th_fill (l, &c, 0, mid, sw - 1, mid, oled () ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 90), LV_OPA_COVER, 0);
	for (int x = 0; s_V.ncols > 0 && x < sw; x++)
	{
		int i = x * s_V.ncols / sw;
		int a = mid - s_V.smax[i] * amp / 127, b = mid - s_V.smin[i] * amp / 127;
		if (a > b) { int t = a; a = b; b = t; }
		th_fill (l, &c, x, a, x, b, c_ink (), LV_OPA_COVER, 0);
	}
}

// ---- the machines' panels (in the scope's place). Every drawing is made of filled rects: lines are runs of dots
// (OLED) or of 2-px squares (CLASSIC), so the dot theme keeps its grid.
static const lv_font_t *f_cap (void) { return oled () ? TH->f_micro : &lv_font_montserrat_14; }
static const lv_font_t *f_head (void) { return oled () ? TH->f_small : &lv_font_montserrat_18; }
static int fh (const lv_font_t *f) { return lv_font_get_line_height (f); }
static uint32_t c_blue (int v) { return oled () ? th_oled_secondary (v) : th_mix (TH->sub, 0xFFFFFF, 40 + v); }
static uint32_t c_teal (void) { return oled () ? th_oled_accent (127) : TH->accent; }

static void dot (lv_layer_t *l, const lv_area_t *c, int x, int y, uint32_t rgb)
{
	int t = oled () ? 0 : 1;
	th_fill (l, c, x, y, x + t, y + t, rgb, LV_OPA_COVER, 0);
}
static void line (lv_layer_t *l, const lv_area_t *c, int x0, int y0, int x1, int y1, uint32_t rgb)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1, dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;)
	{
		dot (l, c, x0, y0, rgb);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}
static void frame (lv_layer_t *l, const lv_area_t *c, int x1, int y1, int x2, int y2, uint32_t rgb)
{
	th_fill (l, c, x1, y1, x2, y1, rgb, LV_OPA_COVER, 0);
	th_fill (l, c, x1, y2, x2, y2, rgb, LV_OPA_COVER, 0);
	th_fill (l, c, x1, y1, x1, y2, rgb, LV_OPA_COVER, 0);
	th_fill (l, c, x2, y1, x2, y2, rgb, LV_OPA_COVER, 0);
}
// the panel's first row: a name left, a note right (the head font); returns the row's height
static int head_row (lv_layer_t *l, const lv_area_t *c, int sw, const char *left, const char *right)
{
	const lv_font_t *f = f_head ();
	th_text (l, c, 0, 0, sw, f, c_ink (), LV_TEXT_ALIGN_LEFT, left);
	if (right && *right) th_text (l, c, 0, 0, sw, f_cap (), c_dim (), LV_TEXT_ALIGN_RIGHT, right);
	return fh (f) + (oled () ? 3 : 6);
}

// 6-OP FM: the algorithm as the Rytm's SRC view draws it (core/ui/page_gfx.cpp), bigger: a box per operator with its
// number and its output level as a bar along its foot, carriers on the bottom row standing on the output, each
// modulator above what it modulates, the feedback loop at the side; the operators' frequencies in a row under it
static void fm_draw (lv_layer_t *l, const lv_area_t *c, int sw, int h)
{
	char b[24];
	lv_snprintf (b, sizeof b, s_V.fm_alg >= 0 ? "ALG %d  FB %d" : "", s_V.fm_alg + 1, s_V.fm_fb);
	int top = head_row (l, c, sw, s_V.fm_alg >= 0 ? (s_V.fm_name[0] ? s_V.fm_name : "(NO NAME)") : "NO PATCH", b);
	if (s_V.fm_alg < 0) return;
	const lv_font_t *fc = f_cap ();
	int rowh = fh (fc) + (oled () ? 2 : 4), bus = h - rowh - (oled () ? 4 : 8);
	int ncols = s_V.fm_ncols < 1 ? 1 : s_V.fm_ncols, nlev = s_V.fm_nlev < 1 ? 1 : s_V.fm_nlev;
	int pitch_x = sw / (ncols > 4 ? ncols : 4), bw = pitch_x * 3 / 5;
	int pitch_y = (bus - top - 2) / nlev, bh = pitch_y * 3 / 5;
	if (bw > (oled () ? 28 : 44)) bw = oled () ? 28 : 44;
	if (bh > (oled () ? 19 : 34)) bh = oled () ? 19 : 34;
	int x0 = (sw - (ncols - 1) * pitch_x - bw) / 2;
	int bx[6], by[6];
	for (int k = 0; k < 6; k++)
	{
		bx[k] = x0 + s_V.fm_col[k] * pitch_x;
		by[k] = bus - 2 - bh - s_V.fm_lev[k] * pitch_y;
	}
	uint32_t wire = oled () ? th_mix (0, c_dim (), 200) : c_dim ();
	for (int t = 0; t < 6; t++)		// the connections: a modulator's foot to its target's top
		for (int m = 0; m < 6; m++)
			if (s_V.fm_mod[t] >> m & 1) line (l, c, bx[m] + bw / 2, by[m] + bh, bx[t] + bw / 2, by[t] - 1, wire);
	int c0 = sw, c1 = -1;
	for (int k = 0; k < 6; k++)
	{
		int car = s_V.fm_car >> k & 1;
		th_fill (l, c, bx[k], by[k], bx[k] + bw - 1, by[k] + bh - 1, c_bg (), LV_OPA_COVER, 0);
		frame (l, c, bx[k], by[k], bx[k] + bw - 1, by[k] + bh - 1, car ? c_ink () : c_blue (110));
		lv_snprintf (b, sizeof b, "%d", k + 1);
		th_text (l, c, bx[k], by[k] + (oled () ? 3 : 3), bw, fc, car ? c_ink () : c_blue (127), LV_TEXT_ALIGN_CENTER, b);
		int in = oled () ? 3 : 5, lw = (bw - 2 * in) * s_V.fm_out[k] / 99, ly = by[k] + bh - (oled () ? 4 : 9);
		if (lw > 0) th_fill (l, c, bx[k] + in, ly, bx[k] + in + lw - 1, ly + (oled () ? 0 : 3), c_teal (), LV_OPA_COVER, 0);
		if (car)
		{
			th_fill (l, c, bx[k] + bw / 2, by[k] + bh, bx[k] + bw / 2 + (oled () ? 0 : 1), bus, c_ink (), LV_OPA_COVER, 0);
			if (bx[k] + bw / 2 < c0) c0 = bx[k] + bw / 2;
			if (bx[k] + bw / 2 > c1) c1 = bx[k] + bw / 2;
		}
	}
	if (c1 >= 0)				// the output and its arrow's head
	{
		int t = oled () ? 0 : 1, ah = oled () ? 3 : 6;
		th_fill (l, c, c0, bus, c1 + ah * 2, bus + t, c_ink (), LV_OPA_COVER, 0);
		for (int i = 1; i <= ah; i++) th_fill (l, c, c1 + ah * 2 - i, bus - i, c1 + ah * 2 - i, bus + t + i, c_ink (), LV_OPA_COVER, 0);
	}
	int src = s_V.fm_fb_src, dst = s_V.fm_fb_dst;
	if (src >= 0 && dst >= 0 && src < 6 && dst < 6)	// feedback: a loop on the right of the box(es)
	{
		int g = oled () ? 3 : 6;
		int xr = (bx[src] > bx[dst] ? bx[src] : bx[dst]) + bw - 1 + g;
		int ya = by[dst] - g, yb = by[src] + bh - 1 + g / 2;
		if (ya < top) ya = top;
		uint32_t fc2 = c_teal ();
		th_fill (l, c, bx[dst] + bw / 2, ya, xr, ya, fc2, LV_OPA_COVER, 0);
		th_fill (l, c, bx[dst] + bw / 2, ya, bx[dst] + bw / 2, by[dst] - 1, fc2, LV_OPA_COVER, 0);
		th_fill (l, c, xr, ya, xr, yb, fc2, LV_OPA_COVER, 0);
		th_fill (l, c, bx[src] + bw / 2, yb, xr, yb, fc2, LV_OPA_COVER, 0);
		th_fill (l, c, bx[src] + bw / 2, by[src] + bh, bx[src] + bw / 2, yb, fc2, LV_OPA_COVER, 0);
	}
	// the frequencies: operator 1..6 left to right, the number dim
	int cw = sw / 6, ty = h - rowh + (oled () ? 2 : 2);
	for (int k = 0; k < 6; k++)
	{
		lv_snprintf (b, sizeof b, "%d", k + 1);
		th_text (l, c, k * cw, ty, cw, fc, c_blue (127), LV_TEXT_ALIGN_LEFT, b);
		th_text (l, c, k * cw + (oled () ? 8 : 12), ty, cw - (oled () ? 8 : 12), fc, c_ink (), LV_TEXT_ALIGN_LEFT, s_V.fm_freq[k]);
	}
}

// MULTISAMPLE: the instrument's name, its zones as bars over a keyboard of its span (a key sounding lit in the teal,
// its zone white), the roots as ticks, then ATK DEC SUS REL (P5..P8) as the envelope
static int key_black (int k) { int p = k % 12; return p == 1 || p == 3 || p == 6 || p == 8 || p == 10; }
static int key_on (int k) { return k >= 0 && k < 128 && (s_V.ms_keys[k >> 3] >> (k & 7) & 1); }
static void multi_draw (lv_layer_t *l, const lv_area_t *c, int sw, int h)
{
	char b[40];
	if (s_V.ms_ready) lv_snprintf (b, sizeof b, oled () ? "%d ZONE%s  TAP: BANKS" : "%d zone%s   tap: banks", s_V.ms_nz,
				       s_V.ms_nz == 1 ? "" : oled () ? "S" : "s");
	else lv_snprintf (b, sizeof b, "%s", s_V.ms_inst[0] ? "LOADING" : "");
	int top = head_row (l, c, sw, s_V.ms_inst[0] ? s_V.ms_inst : "NO INSTRUMENT", b);
	// the span: the zones' keys in whole octaves (C1..C6 with none known)
	int k0 = 127, k1 = 0;
	for (int z = 0; z < s_V.ms_nz; z++) { if (s_V.ms_lo[z] < k0) k0 = s_V.ms_lo[z]; if (s_V.ms_hi[z] > k1) k1 = s_V.ms_hi[z]; }
	for (int k = 0; k < 128; k++) if (key_on (k)) { if (k < k0) k0 = k; if (k > k1) k1 = k; }
	if (k1 < k0) k0 = 24, k1 = 84;
	k0 = k0 / 12 * 12, k1 = k1 / 12 * 12 + 11;
	if (k1 - k0 < 35) k1 = k0 + 35 > 127 ? 127 : k0 + 35;
	int n = k1 - k0 + 1;
	int envh = (h - top) * 2 / 7, kh = (h - top - envh) * 2 / 5, zh = h - top - envh - kh - (oled () ? 5 : 10);
	int ky = top + zh + (oled () ? 2 : 4), ey = ky + kh + (oled () ? 3 : 6);
	// x of key k's left and right edge. CLASSIC: keys equally wide (a sampler's keys are notes, not the piano's look).
	// OLED: the piano's layout - seven white keys an octave, the black keys narrower over their gaps - which the finer
	// grid draws cleanly (white keys 7-9 dots, black 4-5)
	static const unsigned char kWi[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
	const int piano = oled (), nw = (n / 12) * 7, base = (k0 / 12) * 7;
	const long ww256 = (long) sw * 256 / (nw ? nw : 1), bw = (int) (ww256 * 3 / 5 / 256) | 1;
#define WI(k) ((k) / 12 * 7 + kWi[(k) % 12] - base)
#define WX(i) ((int) ((i) * ww256 / 256))
#define KX(k) (piano ? (key_black (k) ? WX (WI (k) + 1) - bw / 2 : WX (WI (k))) : (int) ((long) ((k) - k0) * sw / n))
#define KR(k) (piano ? (key_black (k) ? WX (WI (k) + 1) - bw / 2 + bw - 1 : WX (WI (k) + 1) - 1) : (int) ((long) ((k) + 1 - k0) * sw / n) - 1)
	// the zones: two rows alternating so neighbours read apart; height = its velocity layers (up to 4)
	for (int z = 0; z < s_V.ms_nz; z++)
	{
		int xa = KX (s_V.ms_lo[z]), xb = KR (s_V.ms_hi[z]) - (oled () ? 1 : 1);
		if (xb < xa) xb = xa;
		int half = zh / 2, y1 = (z & 1) ? 0 : half, layers = s_V.ms_vel[z] > 4 ? 4 : s_V.ms_vel[z];
		int on = 0;
		for (int k = s_V.ms_lo[z]; k <= s_V.ms_hi[z]; k++) on |= key_on (k);
		int bh2 = half - 1 - (4 - layers) * (half - 1) / 6;
		th_fill (l, c, xa, top + y1 + (half - bh2), xb, top + y1 + half - 1, on ? c_ink () : c_blue (40 + 20 * layers), LV_OPA_COVER, 0);
		int rx = piano ? (KX (s_V.ms_root[z]) + KR (s_V.ms_root[z])) / 2 : KX (s_V.ms_root[z]) + (KX (s_V.ms_root[z] + 1) - KX (s_V.ms_root[z])) / 2;
		th_fill (l, c, rx, top + y1, rx + (oled () ? 0 : 1), top + y1 + half - 1, on ? c_teal () : c_ink (), LV_OPA_COVER, 0);
	}
	// the keyboard
	if (piano)
	{
		for (int k = k0; k <= k1; k++)		// the white keys, a dark dot between them
		{
			if (key_black (k)) continue;
			int xa = KX (k), xb = KR (k) - 1;
			th_fill (l, c, xa, ky, xb, ky + kh - 1, key_on (k) ? c_teal () : k % 12 == 0 ? 0xC8C8C8 : 0x9A9A9A, LV_OPA_COVER, 0);
		}
		for (int k = k0; k <= k1; k++)		// the black keys over them, two thirds down, a black edge round
		{
			if (!key_black (k)) continue;
			int xa = KX (k), xb = KR (k);
			th_fill (l, c, xa - 1, ky, xb + 1, ky + kh * 3 / 5, 0x000000, LV_OPA_COVER, 0);
			th_fill (l, c, xa, ky, xb, ky + kh * 3 / 5 - 1, key_on (k) ? c_teal () : 0x3A3A3A, LV_OPA_COVER, 0);
		}
	}
	else
	for (int k = k0; k <= k1; k++)
	{
		int xa = KX (k), xb = KX (k + 1) - 1;
		if (xb < xa) xb = xa;
		uint32_t col = key_on (k) ? c_teal () : key_black (k) ? th_mix (c_bg (), c_dim (), 60) : th_mix (c_bg (), c_dim (), 170);
		th_fill (l, c, xa, ky + (key_black (k) ? 0 : kh / 3), xb, ky + kh - 1, col, LV_OPA_COVER, 0);
		if (k % 12 == 0 && k > k0) th_fill (l, c, xa, ky, xa, ky + kh - 1, c_ink (), LV_OPA_COVER, 0);	// each C
	}
	for (int k = k0; k <= k1; k += 12)	// chrorange: each C named at its foot (C4 = 60, as the SEQ pages), dark on the key
	{
		if (k % 12) continue;
		char nm[6];
		lv_snprintf (nm, sizeof nm, "C%d", k / 12 - 1);
		int xa = KX (k) + (piano ? 1 : 2), tw = (piano ? KR (k + 4 > k1 ? k1 : k + 4) : KX (k + 12 > k1 ? k1 : k + 12)) - xa;
		int nw = th_text_width (TH->f_micro, nm), ty = ky + kh - fh (TH->f_micro) - 1;
		if (nw > tw) continue;	// over C..E's feet (keys too narrow for it: none)
		if (piano) th_text (l, c, xa, ty, tw, TH->f_micro, 0x101010, LV_TEXT_ALIGN_LEFT, nm);	// dark on the white keys
		else	// CLASSIC: its black keys run the keyboard's height - the name on a tag of the background, in its ink
		{
			th_fill (l, c, xa - 1, ty, xa + nw, ky + kh - 1, c_bg (), LV_OPA_COVER, 0);
			th_text (l, c, xa, ty, tw, TH->f_micro, c_ink (), LV_TEXT_ALIGN_LEFT, nm);
		}
	}
#undef KR
#undef WX
#undef WI
#undef KX
	// the envelope: ATK DEC SUS REL, each as wide as its knob is high, the sustain held a quarter
	int a = s_V.val[4], d = s_V.val[5], su = s_V.val[6], r = s_V.val[7];
	int q = sw / 4, xa = a * q / 127, xd = xa + d * q / 127, xs = xd + q, xr = xs + r * q / 127;
	int yb = ey + envh - 1, yt = ey, ys = yb - su * (envh - 1) / 127;
	th_fill (l, c, 0, yb, sw - 1, yb, oled () ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 90), LV_OPA_COVER, 0);
	line (l, c, 0, yb, xa, yt, c_ink ());
	line (l, c, xa, yt, xd, ys, c_ink ());
	line (l, c, xd, ys, xs, ys, c_ink ());
	line (l, c, xs, ys, xr, yb, c_ink ());
}

// DAISYSP: the model's name, then MODAL / RESON the resonator's modes as lines on an octave scale (1x .. 32x the
// fundamental, the octaves dotted), each as tall as it rings (DMP, BRI), the fundamental white; STRING the string's
// shape: its harmonics (as many as BRI lets through) at three moments, fading as DMP damps them
static void daisy_draw (lv_layer_t *l, const lv_area_t *c, int sw, int h)
{
	static const char *const kModel[3] = {"MODAL", "STRING", "RESON"};
	int m = s_V.dz_model < 0 ? 0 : s_V.dz_model > 2 ? 2 : s_V.dz_model;
	char b[24];
	lv_snprintf (b, sizeof b, m == 1 ? "%d HARMONICS" : "%d MODES", m == 1 ? 1 + s_V.val[2] * 7 / 127 : s_V.dz_n);
	int top = head_row (l, c, sw, kModel[m], b) + (oled () ? 2 : 4);
	int base = h - 1, ah = base - top;
	if (m != 1)
	{
		for (int o = 0; o <= 5; o++)		// the octaves 1x 2x .. 32x
			th_fill (l, c, o * (sw - 1) / 5, top, o * (sw - 1) / 5, base,
				 oled () ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 70), LV_OPA_COVER, 0);
		th_fill (l, c, 0, base, sw - 1, base, c_dim (), LV_OPA_COVER, 0);
		for (int i = s_V.dz_n - 1; i >= 0; i--)
		{
			float r = s_V.dz_ratio[i] / 100.0f;
			if (r < 1.0f || r > 32.0f) continue;
			int x = (int) (log2f (r) / 5.0f * (sw - 1) + 0.5f);
			int y = base - s_V.dz_ring[i] * ah / 255;
			uint32_t col = i == 0 ? c_ink () : m == 2 ? c_teal () : c_blue (40 + s_V.dz_ring[i] * 87 / 255);
			th_fill (l, c, x, y, x + (oled () ? 0 : 2), base - 1, col, LV_OPA_COVER, 0);
		}
		return;
	}
	// STRING: the string's standing wave, y(u) = sum over harmonics k = 1..H (H from BRI) of sin(k pi u) cos(k w t) / k,
	// at three moments t, each later one smaller (DMP: up = shorter) and dimmer; STR stretches the partials (stiffness)
	int nh = 1 + s_V.val[2] * 7 / 127, mid = top + ah / 2;
	float damp = 0.15f + 0.45f * s_V.val[3] / 127.0f, stretch = 1.0f + 0.04f * (s_V.val[1] > 32 ? s_V.val[1] - 32 : 0) / 95.0f;
	th_fill (l, c, 0, mid, sw - 1, mid, oled () ? DOT_DOTTED : th_mix (c_bg (), c_dim (), 90), LV_OPA_COVER, 0);
	float norm = 0;
	for (int k = 1; k <= nh; k++) norm += 1.0f / k;
	for (int t = 2; t >= 0; t--)
	{
		float amp = (ah / 2 - 2) * powf (1.0f - damp, (float) t) / norm;
		uint32_t col = t == 0 ? c_ink () : c_blue (t == 1 ? 100 : 55);
		int py = mid;
		for (int x = 0; x < sw; x++)
		{
			float u = (float) x / (sw - 1), v = 0, fk = 1.0f;
			for (int k = 1; k <= nh; k++, fk *= stretch)
				v += sinf (k * 3.14159265f * u) * cosf (k * fk * 1.1f * t) / k;
			int y = mid - (int) (amp * v);
			if (x) line (l, c, x - 1, py, x, y, col);
			py = y;
		}
	}
}

static void mode_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int h = lv_area_get_height (&c), sw = level_draw (l, &c);
	if (s_V.mode == SVV_FM) fm_draw (l, &c, sw, h);
	else if (s_V.mode == SVV_MULTI) multi_draw (l, &c, sw, h);
	else if (s_V.mode == SVV_DAISY) daisy_draw (l, &c, sw, h);
}

static void heat_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), n = s_V.steps;
	if (n <= 0)
	{
		th_text (l, &c, 0, (h - (oled () ? 9 : 8)) / 2, w, oled () ? TH->f_micro : &lv_font_montserrat_14, c_dim (), LV_TEXT_ALIGN_CENTER,
			 "NO PATTERN YET (THE RYTM'S DUMP)");
		return;
	}
	if (n > SVV_STEPS) n = SVV_STEPS;
	int max = 1;
	for (int s = 0; s < n; s++) if (s_V.locks[s] > max) max = s_V.locks[s];
	for (int s = 0; s < n; s++)
	{
		int x0 = s * w / n, x1 = (s + 1) * w / n - (oled () ? 1 : 2);
		uint32_t base = s_V.trig[s] ? th_mix (c_bg (), c_dim (), 140) : (oled () ? 0x262626 : th_mix (c_bg (), c_dim (), 50));
		th_fill (l, &c, x0, 0, x1, h - 1, base, LV_OPA_COVER, 0);
		if (s_V.locks[s])
		{
			int t = 96 + 160 * s_V.locks[s] / max;
			th_fill (l, &c, x0, 0, x1, h - 1, th_mix (c_bg (), TH->lock, t > 256 ? 256 : t), LV_OPA_COVER, 0);
		}
		if (s % 16 == 0 && s) th_fill (l, &c, x0, 0, x0, h - 1, c_ink (), LV_OPA_COVER, 0);
	}
}

static void tap_cb (lv_event_t *e)
{
	int what = (int) (intptr_t) lv_event_get_user_data (e);
	if (what == SIDECAR_VOICE_TAP_SOURCES && s_V.mode != SVV_MULTI) return;	// (FM / DAISYSP: nothing)
	if (s_Tap) s_Tap (what);
}

void sidecar_voice_create (void *parent, int y0)
{
	int o = oled ();
	s_nGen = ~0u;
	memset (&s_V, 0, sizeof s_V);
	// 1024x600 (TH->dx, dy): the knobs wider apart and taller, the machine's panel (keyboard, algorithm, scope) gets most
	// of the extra room, the heatmap the full width
	const int dx = TH->dx, dy = TH->dy;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, o ? TH->w - 48 : TH->w, o ? TH->h : 428 + dy);
	s_Page = pg;
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, 352 + dx, 13);
		th_titlebar_set (s_TitleBar, "VOICE", "", 0);
		s_Title = 0;
		// the finer grid: the eight knobs in one row, the machine's panel the full width under them (the MULTISAMPLE
		// keyboard's white keys 8 dots wide, the FM algorithm's boxes and wires with room), the p-locks at the foot
		const int kp = (352 + dx) / 8, ky = dy / 6;
		s_Name = th_label (pg, 3, 16, TH->f_body, TH->ink, "");
		for (int k = 0; k < 8; k++) s_Knob[k] = th_meter (pg, k * kp, 29, kp - 1, 52 + ky, 127, TH_METER_KNOB, TH->ink);
		s_Scope = th_box (pg, 3, 91 + ky, 349 + dx, 104 + (dy - ky) * 4 / 5);
		s_Mode = th_box (pg, 3, 87 + ky, 349 + dx, 110 + (dy - ky) * 4 / 5);
		s_HeatCap = th_label (pg, 3, 205 + dy, TH->f_micro, TH->dim, "P-LOCKS");
		s_Heat = th_box (pg, 50, 202 + dy, 302 + dx, 16);
		s_Info = th_label (pg, 3, 226 + dy, TH->f_micro, TH->dim, "");
	}
	else
	{
		const int kp = 100 + dx / 16, kh = 124 + dy / 4, sx = 440 + dx / 4;
		s_TitleBar = 0;
		th_panel (pg, 8, 6, 784 + dx, 414 + dy, "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, 24, 14, &lv_font_montserrat_18, TH->win_ink, "VOICE");
		s_Name = th_label (pg, 24, 40, &lv_font_montserrat_24, TH->win_ink, "");
		for (int k = 0; k < 8; k++) s_Knob[k] = th_meter (pg, 24 + (k & 3) * kp, 84 + (k >> 2) * (kh + 6), kp - 8, kh, 127, TH_METER_KNOB, TH->win_ink);
		s_Scope = th_box (pg, sx, 84, 336 + dx * 3 / 4, 250 + dy * 3 / 4);
		s_Mode = th_box (pg, sx, 80, 336 + dx * 3 / 4, 258 + dy * 3 / 4);
		s_HeatCap = th_label (pg, 24, 348 + dy, &lv_font_montserrat_14, TH->win_dim, "P-LOCKS (THE RYTM'S PATTERN)");
		s_Heat = th_box (pg, 24, 370 + dy, 752 + dx, 30);
		s_Info = th_label (pg, sx, 14, &lv_font_montserrat_14, TH->win_dim, "");
	}
	lv_obj_set_width (s_Info, (o ? 346 : 330) + dx * 3 / 4);
	lv_obj_add_event_cb (s_Scope, scope_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Mode, mode_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_flag (s_Mode, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb (s_Heat, heat_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_flag (s_Scope, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Scope, tap_cb, LV_EVENT_CLICKED, (void *) (intptr_t) SIDECAR_VOICE_TAP_SCOPE);
	lv_obj_add_flag (s_Mode, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Mode, tap_cb, LV_EVENT_CLICKED, (void *) (intptr_t) SIDECAR_VOICE_TAP_SOURCES);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_voice_set_tap (void (*fn) (int what)) { s_Tap = fn; }

void sidecar_voice_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_voice_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

void sidecar_voice_set (const sidecar_voice_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	int texts = v->track != s_V.track || v->machine != s_V.machine || strcmp (v->name, s_V.name) || v->valid != s_V.valid ||
		    v->pattern != s_V.pattern || v->sidecar != s_V.sidecar;
	int knobs = texts || memcmp (v->val, s_V.val, sizeof v->val) || memcmp (v->label, s_V.label, sizeof v->label);
	int scope = v->ncols != s_V.ncols || v->level != s_V.level || memcmp (v->smin, s_V.smin, sizeof v->smin) ||
		    memcmp (v->smax, s_V.smax, sizeof v->smax);
	int heat = v->steps != s_V.steps || memcmp (v->locks, s_V.locks, sizeof v->locks) || memcmp (v->trig, s_V.trig, sizeof v->trig);
	int mode = v->mode != s_V.mode || (v->mode != SVV_PLAIN && (knobs || v->level != s_V.level ||
		   memcmp ((const char *) v + offsetof (sidecar_voice_view_t, fm_name), (const char *) &s_V + offsetof (sidecar_voice_view_t, fm_name),
			   offsetof (sidecar_voice_view_t, gen) - offsetof (sidecar_voice_view_t, fm_name))));
	int swap = v->mode != s_V.mode;
	s_V = *v;
	s_nGen = v->gen;
	if (swap)				// the machine changed: its panel or the scope (the page follows it live)
	{
		if (v->mode != SVV_PLAIN) { lv_obj_add_flag (s_Scope, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag (s_Mode, LV_OBJ_FLAG_HIDDEN); }
		else { lv_obj_add_flag (s_Mode, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag (s_Scope, LV_OBJ_FLAG_HIDDEN); }
	}
	if (texts)
	{
		// UX review T7: the title names the track and its machine ("VOICE  RS  6-OP FM"; voice_pi puts the shared long
		// name in v->name for a Sidecar machine), the name line the Rytm's own name
		char t[48], r[32];
		const char *tn = v->track >= 0 && v->track < 12 ? kTrack[v->track] : "-";
		if (v->valid) lv_snprintf (r, sizeof r, "%s  %s", tn, v->sidecar ? v->name : "ANALOG");
		else lv_snprintf (r, sizeof r, "-");
		if (s_TitleBar) th_titlebar_set (s_TitleBar, "VOICE", r, 0);
		else { lv_snprintf (t, sizeof t, "VOICE  %s", r); th_label_set (s_Title, t); }
		th_label_set (s_Name, v->valid ? v->name : "WAITING FOR THE RYTM");
		if (v->pattern >= 0) lv_snprintf (t, sizeof t, "PATTERN %c%02d  UP + T4: HERE", 'A' + v->pattern / 16, v->pattern % 16 + 1);
		else lv_snprintf (t, sizeof t, "UP + T1..T8 ON THE RYTM: PAGES");
		th_label_set (s_Info, t);
	}
	if (knobs)
		for (int k = 0; k < 8; k++)
		{
			int used = v->valid && v->label[k][0];
			char nb[8];
			lv_snprintf (nb, sizeof nb, "%u", v->val[k]);
			th_meter_set (s_Knob[k], used ? v->val[k] : -1);
			th_meter_label (s_Knob[k], used ? v->label[k] : "");
			th_meter_value (s_Knob[k], used ? nb : v->valid ? "" : "---");	// UX review T7: the value; waiting: ---
		}
	if (scope && v->mode == SVV_PLAIN) lv_obj_invalidate (s_Scope);
	if (mode) lv_obj_invalidate (s_Mode);
	if (heat) lv_obj_invalidate (s_Heat);
}
