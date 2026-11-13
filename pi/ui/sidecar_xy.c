// pi/ui/sidecar_xy.c - see sidecar_xy.h. The pad left (code-drawn: a quarter grid, the finger's dot with a short trail,
// a held position as a ring), the HOLD and SETUP keys right over the slots' summary. SETUP puts X's and Y's slots in the
// pad's place (a target key and the depth's value over a depth slider, a slot); a target key opens the picker there.
// No fixed places: from the page's width and height.
#include "sidecar_xy.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

#define TRAIL	8		// the trail's points

// ---- the targets the picker offers (core/mod/target.h's kinds, by their names), and MORPH
typedef struct
{
	const char		*label;		// the picker's key
	const char		*pre;		// the name's prefix ("V" -> V3:P5); 0: inames
	int			ni;		// indices (0: a single one, named without an index: "RYTM:PERF3")
	const char *const	*inames;	// the indices' own names (INSERT's chains)
	const char *const	*pn;		// the params' names; 0: "P<n>" from pbase
	int			np, pbase;
} xy_kind_t;
static const char *const kPerfP[] = {"PERF1", "PERF2", "PERF3", "PERF4", "PERF5", "PERF6", "PERF7", "PERF8", "PERF9", "PERF10", "PERF11", "PERF12"};
static const char *const kScnP[] = {"SCN"};
static const char *const kSndP[] = {"F1", "F2"};
static const char *const kLoopP[] = {"BAL", "LEV"};
static const char *const kChains[] = {"I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8", "B1", "B2", "B3", "M1", "M2"};
static const char *const kInsP[] = {"A1", "A2", "A3", "A4", "B1", "B2", "B3", "B4", "E1", "E2", "E3", "E4"};
static const char *const kIsndP[] = {"B1", "B2", "B3"};
static const char *const kMfxP[] = {"LEV", "MIX1", "MIX2", "MIX3", "MIX4", "B1A", "B1B", "B2A", "B2B", "B3A", "B3B"};
static const char *const kOutP[] = {"LEV", "PAN", "WID"};
static const char *const kSeqP[] = {"SPD", "DIR", "SWG", "LEN", "RNDM", "RLOW", "ROCT", "RWGT"};
static const char *const kArpP[] = {"MODE", "SPD", "RNG", "GATE", "SWNG"};
// 0.5.0: the Rytm's own sound parameters (core/mod/target.h kRs / kRa / kRfx, docs/RYTM-PARAMS.md): the words an
// offset can move (core/lfo.cpp kRtWords / kRfxWords: no list words), by their names
static const char *const kRtP[] = {"SPD", "FAD", "SPH", "DEP", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "TUN", "FIN",
				   "BR", "STA", "END", "LEV", "FENV", "FATK", "FDEC", "FSUS", "FREL", "FRQ", "RES", "ATK",
				   "HLD", "DEC", "OVR", "VOL", "PAN", "ACC", "DEL", "REV", "VM1", "VM2", "VM3", "VM4", "AT1",
				   "AT2", "AT3", "AT4", "VEL"};
static const char *const kRfxP[] = {"SPD", "FAD", "DEP", "TIM", "X", "WID", "FDB", "DHPF", "DLPF", "DREV", "DVOL", "DOV",
				    "PRE", "RDEC", "RFRQ", "GAI", "RHPF", "RLPF", "RVOL", "AMT", "SYM", "THR", "MUP", "MIX",
				    "CVOL"};
// 0.5.0 (owner): ONE entry per Rytm track (K_TRK: BD SD RS CP BT LT MT HT CH OH CY CB) in place of VOICE and RT. A track's
// params are one list: the Sidecar voice's P1..P16 (the wire's V<n>:P<k>) when the track plays a Sidecar machine, then
// the track's own Rytm params (RT<t>:<name>). Display only: the wire names are what they were. A V<n> name shows under
// the first track of voice n that plays a Sidecar machine, else the first track.
enum { K_PERF, K_MORPH, K_SCN, K_TRK, K_FX, K_SEND, K_LOOP, K_INS, K_BUS, K_MFX, K_OUT, K_SEQ, K_ARP, K_RFX, K_N };
static const char *const kTrkN[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
static const unsigned char kTrkVoice[12] = {0, 2, 7, 7, 1, 3, 5, 5, 4, 4, 6, 6};	// core/ui/remote.cpp kTrackVoice
#define TRK_RT 43	// kRtP's length
static const xy_kind_t kKinds[K_N] = {
	{"PERF", "RYTM", 0, 0, kPerfP, 12, 0},
	{"MORPH", 0, 0, 0, 0, 0, 0},
	{"SCN", "RYTM", 0, 0, kScnP, 1, 0},
	{"TRACK", "T", 12, kTrkN, 0, 16 + TRK_RT, 1},
	{"FX", "FX", 2, 0, 0, 4, 2},
	{"SEND", "S", 12, 0, kSndP, 2, 0},
	{"LOOP", "LOOP", 0, 0, kLoopP, 2, 0},
	{"INS", 0, 13, kChains, kInsP, 12, 0},
	{"BUS", "X", 8, 0, kIsndP, 3, 0},
	{"MFX", "MFX", 0, 0, kMfxP, 11, 0},
	{"OUT", "O", 8, 0, kOutP, 3, 0},
	{"SEQ", "T", 12, 0, kSeqP, 8, 0},
	{"ARP", "R", 12, 0, kArpP, 5, 0},
	{"RFX", "RFX", 0, 0, kRfxP, 25, 0},
};
static const char *const kKindMap[] = {"PERF", "MORPH", "SCN", "TRACK", "FX", "SEND", "LOOP", "\n",
				       "INS", "BUS", "MFX", "OUT", "SEQ", "ARP", "RFX", ""};
static int kind_of_item (int i) { return i; }	// (the map's items in kKinds' order; "\n" is not an item)

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Pad, *s_Hold, *s_SetupK, *s_Pos, *s_Sum[2 * SXY_SLOTS], *s_Info;
static lv_obj_t *s_Setup, *s_AxT[2], *s_SlotK[2][SXY_SLOTS], *s_SlotV[2][SXY_SLOTS], *s_SlotS[2][SXY_SLOTS];
static lv_obj_t *s_Pick, *s_PickT, *s_Kinds, *s_IdxL, *s_IdxM, *s_IdxP, *s_ParL, *s_ParM, *s_ParP, *s_PSet, *s_PNone;
static sidecar_xy_view_t s_V;
static unsigned s_nGen = ~0u;
static sidecar_xy_command_t s_Cmd;
static int s_bSetup, s_nPickA = -1, s_nPickS, s_nKind, s_nIdx, s_nPar;
static int s_nVForce;	// track + 1: a stored V<n> name on a track the Rytm says is stock: its voice params stay listed
// TRACK's list: P1..P16 of the track's voice first, then kRtP. A voice shared by a pair (RS/CP ...) lists its params under
// ONE track of the pair, the first that plays a Sidecar machine (not known counts as one: core/lfo.cpp voice_owner), the
// other lists its Rytm params only. A pair with no Sidecar track: none, unless a stored V name sits on its first track.
static int voice_owner (int v)
{
	for (int t = 0; t < 12; t++) if (kTrkVoice[t] == v && s_V.tm[t] != 2) return t;
	return -1;
}
static int trk_off (int i) { i %= 12; return voice_owner (kTrkVoice[i]) == i || s_nVForce == i + 1 ? 16 : 0; }
static int kind_np (int k, int i) { return k == K_TRK ? trk_off (i) + TRK_RT : kKinds[k].np ? kKinds[k].np : 1; }
static int s_bDown, s_nX, s_nY, s_nTrail, s_TrX[TRAIL], s_TrY[TRAIL];
static int s_bPend;			// a move the throttle held back: sent once its 30 ms are up, moving or not
static uint32_t s_nStuck;		// since when the engine says touching with no finger here (0: it does not)
static uint32_t s_nUpAt;		// the last xy=up sent for that
static uint32_t s_nHoldAt;		// HOLD tapped (0: not lately): the engine's view must confirm it
static int s_PadX, s_PadY, s_PadW, s_PadH;	// the pad in page coordinates (sidecar_xy_pad_area)

static int oled (void) { return TH->id == THEME_OLED; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static int any_slot (void)
{
	for (int a = 0; a < 2; a++) for (int i = 0; i < SXY_SLOTS; i++) if (s_V.s[a][i].name[0]) return 1;
	return 0;
}

// ---- a kind's names
static void kind_name (int k, int i, int p, char *b, int cap)
{
	const xy_kind_t *d = &kKinds[k];
	char pn[8];
	if (k == K_MORPH) { lv_snprintf (b, cap, "MORPH"); return; }
	if (k == K_TRK) pn[0] = 0;
	else if (d->pn) lv_snprintf (pn, sizeof pn, "%s", d->pn[p]); else lv_snprintf (pn, sizeof pn, "P%d", p + d->pbase);
	if (k == K_TRK)
	{
		int o = trk_off (i);
		if (p < o) lv_snprintf (b, cap, "V%d:P%d", kTrkVoice[i % 12] + 1, p + 1);
		else lv_snprintf (b, cap, "RT%d:%s", i % 12 + 1, kRtP[(p - o) % TRK_RT]);
		return;
	}
	if (!d->ni) lv_snprintf (b, cap, "%s:%s", d->pre, pn);
	else if (d->inames) lv_snprintf (b, cap, "%s:%s", d->inames[i], pn);
	else lv_snprintf (b, cap, "%s%d:%s", d->pre, i + 1, pn);
}
// a param as the engine labels it now (the view's tables: VOICE by its machine, FX by its TYPE, INS by its slots' TYPEs),
// the fixed names for the rest; "" when the view has none (before the first view)
static const char *param_label (int k, int i, int p)
{
	const xy_kind_t *d = &kKinds[k];
	if (k == K_TRK)
	{
		int o = trk_off (i), r = p - o;
		if (p < o) return s_V.vl[kTrkVoice[i % 12]][p & 15];
		if (r >= 4 && r < 12 && s_V.rl[i % 12][r - 4][0]) return s_V.rl[i % 12][r - 4];	// the Rytm's P1..P8: "TUN"
		return kRtP[r % TRK_RT];
	}
	if (k == K_FX) return s_V.fl[i & 1][p & 3];
	if (k == K_INS) return s_V.il[i % 13][p % 12];
	return d->pn ? d->pn[p] : "";
}
static int unused (const char *l) { return !l[0] || !strcmp (l, "-") || (l[0] && !strcmp (l + 1, "-")); }
// the picker's title and the slots' keys: "V1:HRM", "FX1:CUT", "I3:ACUT" ("V1:-": a P the voice's machine leaves unused)
static void kind_label (int k, int i, int p, char *b, int cap)
{
	char n[16];
	kind_name (k, i, p, n, sizeof n);
	if (k == K_TRK)	// "RS:WID" (a voice param by its machine's label), "BD:FRQ"
	{
		const char *tl = param_label (k, i, p);
		lv_snprintf (b, cap, "%s:%s", kTrkN[i % 12], tl[0] ? tl : n);
		return;
	}
	if (k != K_FX && k != K_INS) { lv_snprintf (b, cap, "%s", n); return; }
	const char *l = param_label (k, i, p), *c = strchr (n, ':');
	if (!l[0]) { lv_snprintf (b, cap, "%s", n); return; }	// (no view yet: the id)
	lv_snprintf (b, cap, "%.*s:%s", c ? (int) (c - n) : 0, n, l);
}
static void index_name (int k, int i, char *b, int cap)
{
	const xy_kind_t *d = &kKinds[k];
	if (!d->ni) lv_snprintf (b, cap, "-");
	else if (d->inames) lv_snprintf (b, cap, "%s", d->inames[i]);
	else lv_snprintf (b, cap, "%s%d", d->pre, i + 1);
}
static int find_name (const char *n, int *k, int *i, int *p)	// a name back into the picker's kind, index, param
{
	char b[16];
	s_nVForce = 0;
	if (n[0] == 'V' || !strncmp (n, "RT", 2))	// the wire's V<n>:P<k> / RT<t>:<name>: onto a TRACK entry
	{
		int t = -1, pp0 = 0;
		if (n[0] == 'V')	// the pair's owner, else the voice's first track (its voice part then kept while it is this name)
		{
			int v = atoi (n + 1) - 1;
			const char *pc = strchr (n, 'P');
			if (v < 0 || v > 7 || !pc) return 0;
			t = voice_owner (v);
			if (t < 0) for (int u = 11; u >= 0; u--) if (kTrkVoice[u] == v) t = u;
			if (voice_owner (v) < 0) s_nVForce = t + 1;
			pp0 = atoi (pc + 1) - 1;
		}
		for (int tt = 0; tt < 12; tt++)
			for (int pp = 0; pp < kind_np (K_TRK, tt); pp++)
			{
				kind_name (K_TRK, tt, pp, b, sizeof b);
				if (!strcmp (b, n) && (n[0] == 'R' || tt == t)) { *k = K_TRK, *i = tt, *p = pp; return 1; }
			}
		(void) pp0;
		s_nVForce = 0;
		return 0;
	}
	for (int kk = 0; kk < K_N; kk++)
		for (int ii = 0; ii < (kKinds[kk].ni ? kKinds[kk].ni : 1); ii++)
			for (int pp = 0; pp < (kKinds[kk].np ? kKinds[kk].np : 1); pp++)
			{
				if (kk == K_TRK) continue;
				kind_name (kk, ii, pp, b, sizeof b);
				if (!strcmp (b, n)) { *k = kk, *i = ii, *p = pp; return 1; }
			}
	return 0;
}

// ---- the pad
static void pad_draw (lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target_obj (e);
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	const int O = oled (), w = lv_area_get_width (&c), h = lv_area_get_height (&c), u = O ? 1 : 2;
	if (!O) th_fill (l, &c, 0, 0, w - 1, h - 1, TH->win, LV_OPA_COVER, TH->radius);
	else th_frame (l, &c, 0, 0, w - 1, h - 1, DOT_DOTTED, 1, 0);
	const uint32_t grid = O ? DOT_DOTTED : th_mix (TH->win, TH->win_dim, 70);
	for (int q = 1; q < 4; q++)
	{
		th_fill (l, &c, q * w / 4, 2 * u, q * w / 4, h - 1 - 2 * u, grid, LV_OPA_COVER, 0);
		th_fill (l, &c, 2 * u, q * h / 4, w - 1 - 2 * u, q * h / 4, grid, LV_OPA_COVER, 0);
	}
	const lv_font_t *f = O ? TH->f_micro : TH->f_small;
	const int fh = lv_font_get_line_height (f);
	th_text (l, &c, 3 * u, 2 * u, w / 4, f, c_dim (), LV_TEXT_ALIGN_LEFT, "Y");
	th_text (l, &c, w - w / 4 - 3 * u, h - fh - 2 * u, w / 4, f, c_dim (), LV_TEXT_ALIGN_RIGHT, "X");
	if (s_V.valid && !any_slot ())
		th_text (l, &c, 0, h / 2 - fh / 2, w, O ? TH->f_body : TH->f_mid, c_dim (), LV_TEXT_ALIGN_CENTER,
			 O ? "NO TARGETS: TAP SETUP" : "No targets yet: tap SETUP");
	int have = s_bDown || (s_V.valid && s_V.active);
	if (!have) return;
	int x = s_bDown ? s_nX : s_V.x, y = s_bDown ? s_nY : s_V.y;
	int px = x * (w - 1) / SXY_MAX, py = (h - 1) - y * (h - 1) / SXY_MAX, r = O ? 4 : 10 + TH->dy / 24;
	const uint32_t dot = O ? TH->ink : TH->accent, cross = O ? DOT_DOTTED : th_mix (TH->win, TH->accent, 90);
	th_fill (l, &c, px, 1, px, h - 2, cross, LV_OPA_COVER, 0);
	th_fill (l, &c, 1, py, w - 2, py, cross, LV_OPA_COVER, 0);
	if (s_bDown)		// the trail: the older the smaller and fainter
		for (int i = 0; i < s_nTrail; i++)
		{
			int tx = s_TrX[i] * (w - 1) / SXY_MAX, ty = (h - 1) - s_TrY[i] * (h - 1) / SXY_MAX, tr = 1 + r * (i + 1) / (2 * TRAIL);
			uint32_t tc = O ? DOT_SHADE : th_mix (TH->win, TH->accent, 60 + 140 * (i + 1) / TRAIL);
			th_fill (l, &c, tx - tr, ty - tr, tx + tr, ty + tr, tc, LV_OPA_COVER, tr);
		}
	if (s_bDown || !s_V.hold || s_V.touching) th_fill (l, &c, px - r, py - r, px + r, py + r, dot, LV_OPA_COVER, r);
	else th_frame (l, &c, px - r, py - r, px + r, py + r, dot, u, r);	// HOLD's held position: a ring
}
static void pos_text (void)
{
	char t[32];
	if (s_V.valid && (s_V.active || s_bDown))
	{
		int x = s_bDown ? s_nX : s_V.x, y = s_bDown ? s_nY : s_V.y;
		lv_snprintf (t, sizeof t, "X %d%%  Y %d%%", (x * 100 + SXY_MAX / 2) / SXY_MAX, (y * 100 + SXY_MAX / 2) / SXY_MAX);
	}
	else lv_snprintf (t, sizeof t, "%s", oled () ? "NOT TOUCHED" : "Not touched");
	th_label_set (s_Pos, t);
}
static void send_pos (void)
{
	char c[32];
	lv_snprintf (c, sizeof c, "xy=pos:%d:%d", s_nX, s_nY);
	cmd (c);
}
static void pad_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
	static uint32_t s_nAt;
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
	{
		if (!s_bDown) return;
		send_pos ();		// the last position, then the release
		cmd ("xy=up");
		s_bDown = 0, s_nTrail = 0, s_bPend = 0;
		lv_obj_invalidate (s_Pad);
		return;
	}
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (s_Pad, &c);
	int w = lv_area_get_width (&c), h = lv_area_get_height (&c), x = p.x - c.x1, y = c.y2 - p.y;
	x = x < 0 ? 0 : x > w - 1 ? w - 1 : x;
	y = y < 0 ? 0 : y > h - 1 ? h - 1 : y;
	int nx = x * SXY_MAX / (w - 1), ny = y * SXY_MAX / (h - 1);
	uint32_t now = lv_tick_get ();
	if (code == LV_EVENT_PRESSED) s_bDown = 1, s_nTrail = 0;
	else if (nx == s_nX && ny == s_nY)		// the finger rests: a held-back move goes out when its time comes
	{
		if (s_bPend && now - s_nAt >= 30) s_nAt = now, s_bPend = 0, send_pos ();
		return;
	}
	if (s_nTrail == TRAIL) memmove (s_TrX, s_TrX + 1, sizeof s_TrX[0] * (TRAIL - 1)), memmove (s_TrY, s_TrY + 1, sizeof s_TrY[0] * (TRAIL - 1)), s_nTrail--;
	if (code != LV_EVENT_PRESSED) s_TrX[s_nTrail] = s_nX, s_TrY[s_nTrail] = s_nY, s_nTrail++;
	s_nX = nx, s_nY = ny;
	lv_obj_invalidate (s_Pad);
	pos_text ();
	// live while dragging, at most every 30 ms (each line is a console command on core 3); the touch's first at once
	if (code != LV_EVENT_PRESSED && now - s_nAt < 30) { s_bPend = 1; return; }
	s_nAt = now, s_bPend = 0;
	send_pos ();
}

// ---- the keys
static void refresh (void);
static void hold_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	s_V.hold = !s_V.hold;		// shown now; the engine's view confirms it (else, the command lost, shows its own)
	s_nHoldAt = lv_tick_get () | 1;
	cmd (s_V.hold ? "xy=hold:1" : "xy=hold:0");
	refresh ();
}
static void show_mode (void)
{
	if (s_bSetup && s_nPickA < 0) lv_obj_remove_flag (s_Setup, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Setup, LV_OBJ_FLAG_HIDDEN);
	if (s_bSetup && s_nPickA >= 0) lv_obj_remove_flag (s_Pick, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Pick, LV_OBJ_FLAG_HIDDEN);
	if (!s_bSetup) lv_obj_remove_flag (s_Pad, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Pad, LV_OBJ_FLAG_HIDDEN);
	th_key_text (s_SetupK, s_bSetup ? "PAD" : "SETUP");
	th_key_check (s_SetupK, s_bSetup);
}
static void setup_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) i; (void) ud; sidecar_xy_setup (!s_bSetup); }
static void slot_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int n = (int) (intptr_t) ud;
	sidecar_xy_pick (n / SXY_SLOTS, n % SXY_SLOTS);
}
static void depth_cb (lv_obj_t *o, int v, void *ud)
{
	(void) o;
	int n = (int) (intptr_t) ud, a = n / SXY_SLOTS, k = n % SXY_SLOTS, d = (v & 0xFFFF) - 100;
	char t[8];
	lv_snprintf (t, sizeof t, "%+d", d);
	th_label_set (s_SlotV[a][k], t);
	if (!(v & TH_RELEASED) || !s_V.s[a][k].name[0]) return;	// the depth goes when the finger lifts
	char c[48];
	lv_snprintf (c, sizeof c, "xy=set:%c:%d:%s:%d", a ? 'y' : 'x', k + 1, s_V.s[a][k].name, d);
	cmd (c);
}

// ---- the picker
static void pick_refresh (void)
{
	const xy_kind_t *d = &kKinds[s_nKind];
	char t[40], n[16];
	if (s_nIdx >= (d->ni ? d->ni : 1)) s_nIdx = 0;
	if (s_nPar >= kind_np (s_nKind, s_nIdx)) s_nPar = 0;
	kind_label (s_nKind, s_nIdx, s_nPar, n, sizeof n);
	lv_snprintf (t, sizeof t, "%c%d: %s", s_nPickA ? 'Y' : 'X', s_nPickS + 1, n);
	th_label_set (s_PickT, t);
	th_seg_set (s_Kinds, s_nKind);
	index_name (s_nKind, s_nIdx, t, sizeof t);
	th_label_set (s_IdxL, t);
	th_key_enable (s_IdxM, d->ni > 1), th_key_enable (s_IdxP, d->ni > 1);
	if (s_nKind == K_MORPH) lv_snprintf (t, sizeof t, "THE SCENES' FADER");
	else if (s_nKind == K_INS)	// "A1 CUT", "E1 HPF" (the label's slot letter / Q dropped), "A1 -" (the slot OFF)
	{
		const char *l = param_label (s_nKind, s_nIdx, s_nPar);
		lv_snprintf (t, sizeof t, "%s %s", d->pn[s_nPar], unused (l) ? "-" : l + 1);
	}
	else if (s_nKind == K_TRK)	// "P3 HRM" (the voice's param by its machine), "FRQ", "P2 TUN" (the Rytm's P2 by its machine)
	{
		const char *l = param_label (s_nKind, s_nIdx, s_nPar);
		int o = trk_off (s_nIdx), r = s_nPar - o;
		if (s_nPar < o) lv_snprintf (t, sizeof t, "P%d %s", s_nPar + 1, unused (l) ? "-" : l);
		else if (r >= 4 && r < 12 && strcmp (l, kRtP[r])) lv_snprintf (t, sizeof t, "%s %s", kRtP[r], unused (l) ? "-" : l);
		else lv_snprintf (t, sizeof t, "%s", kRtP[r % TRK_RT]);
	}
	else if (d->pn) lv_snprintf (t, sizeof t, "%s", d->pn[s_nPar]);
	else	// VOICE / FX: "P3 HRM", "P5 -" (unused on the voice's machine / the FX's TYPE)
	{
		const char *l = param_label (s_nKind, s_nIdx, s_nPar);
		lv_snprintf (t, sizeof t, "P%d %s", s_nPar + d->pbase, unused (l) ? "-" : l);
	}
	th_label_set (s_ParL, t);
	th_key_enable (s_ParM, kind_np (s_nKind, s_nIdx) > 1), th_key_enable (s_ParP, kind_np (s_nKind, s_nIdx) > 1);
	th_key_enable (s_PNone, s_nPickA >= 0 && s_V.s[s_nPickA][s_nPickS].name[0]);
}
static void kind_cb (lv_obj_t *o, int i, void *ud) { (void) o; (void) ud; s_nKind = kind_of_item (i), s_nIdx = s_nPar = 0, s_nVForce = 0; pick_refresh (); }
static void step_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int w = (int) (intptr_t) ud, d = w & 1 ? 1 : -1;	// 0 / 1: the index - / +, 2 / 3: the param - / +
	const xy_kind_t *k = &kKinds[s_nKind];
	if (w < 2 && k->ni > 1) s_nIdx = (s_nIdx + d + k->ni) % k->ni, s_nVForce = 0, s_nPar = s_nPar >= kind_np (s_nKind, s_nIdx) ? 0 : s_nPar;
	else if (w >= 2 && kind_np (s_nKind, s_nIdx) > 1) { int np = kind_np (s_nKind, s_nIdx); s_nPar = (s_nPar + d + np) % np; }
	pick_refresh ();
}
static void pick_end_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int w = (int) (intptr_t) ud;	// 0 SET, 1 NONE, 2 BACK
	char c[48], n[16];
	if (s_nPickA >= 0 && w == 0)
	{
		const sidecar_xy_slot_t *sl = &s_V.s[s_nPickA][s_nPickS];
		kind_name (s_nKind, s_nIdx, s_nPar, n, sizeof n);
		lv_snprintf (c, sizeof c, "xy=set:%c:%d:%s:%d", s_nPickA ? 'y' : 'x', s_nPickS + 1, n, sl->name[0] ? sl->depth : 100);
		cmd (c);
	}
	else if (s_nPickA >= 0 && w == 1)
	{
		lv_snprintf (c, sizeof c, "xy=clear:%c:%d", s_nPickA ? 'y' : 'x', s_nPickS + 1);
		cmd (c);
	}
	sidecar_xy_pick (-1, 0);
}

void sidecar_xy_create (void *parent, int y0)
{
	const int o = oled ();
	s_nGen = ~0u, s_bSetup = 0, s_nPickA = -1, s_bDown = 0, s_nTrail = 0;
	memset (&s_V, 0, sizeof s_V);
	const int pw = th_page_w (), ph = TH->h - y0;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph);
	s_Page = pg;
	const lv_font_t *f = o ? TH->f_micro : TH->f_small;
	const int kind = o ? TH_KEY | TH_KEY_SMALL : TH_KEY, ink = o ? TH->ink : TH->win_ink, dim = o ? TH->dim : TH->win_dim;
	const int x0 = o ? 0 : LU (24), w = o ? pw : pw - LU (48), top = o ? LU (16) : LU (44), g = o ? LU (3) : LU (8);
	const int lh = lv_font_get_line_height (f), foot = o ? lh + LU (4) : lh + LU (10), keyh = o ? LU (20) + TH->dy / 12 : LU (40) + TH->dy / 8;
	if (o)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, LU (13));
		th_titlebar_set (s_TitleBar, "XY PAD", "", 0);
		s_Title = 0;
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, LU (8), LU (6), pw - LU (16), ph - LU (14), "", TH_PANEL_WINDOW);
		s_Title = th_label (pg, LU (24), LU (14), TH->f_mid, TH->win_ink, "XY PAD");
	}
	// the right column: HOLD, SETUP, the position, the slots in use
	const int sw = o ? LU (78) : LU (164), bottom = ph - foot - (o ? 0 : LU (14)), ah = bottom - top;
	const int aw = w - sw - g, sx = x0 + aw + g;
	s_PadX = x0, s_PadY = y0 + top, s_PadW = aw, s_PadH = ah;
	s_Hold = th_key (pg, sx, top, sw, keyh, "HOLD", kind | TH_KEY_HOLD, hold_cb, 0);
	s_SetupK = th_key (pg, sx, top + keyh + g, sw, keyh, "SETUP", kind, setup_cb, 0);
	int ly = top + 2 * (keyh + g);
	s_Pos = th_label (pg, sx, ly, f, ink, "");
	lv_obj_set_width (s_Pos, sw);
	ly += lh + g;
	for (int i = 0; i < 2 * SXY_SLOTS; i++)
	{
		s_Sum[i] = th_label (pg, sx, ly + i * lh, f, i < SXY_SLOTS ? ink : ink, "");
		lv_obj_set_width (s_Sum[i], sw);
		if (ly + (i + 1) * lh > bottom) lv_obj_add_flag (s_Sum[i], LV_OBJ_FLAG_HIDDEN);
	}
	s_Info = th_label (pg, x0, bottom + (o ? LU (2) : LU (8)), f, o ? TH->faint : dim, "");
	lv_obj_set_width (s_Info, w);
	// the pad
	s_Pad = th_box (pg, x0, top, aw, ah);
	lv_obj_add_flag (s_Pad, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb (s_Pad, pad_draw, LV_EVENT_DRAW_MAIN, 0);
	lv_obj_add_event_cb (s_Pad, pad_cb, LV_EVENT_ALL, 0);
	// SETUP: X left, Y right; a slot: its target key and the depth's value, the depth slider under them
	s_Setup = th_box (pg, x0, top, aw, ah);
	const int cw = (aw - 2 * g) / 2, vw = o ? LU (26) : LU (56), rh = (ah - lh - g) / SXY_SLOTS, kh = rh / 2 - (o ? 1 : 3);
	for (int a = 0; a < 2; a++)
	{
		int cx = a * (cw + 2 * g);
		s_AxT[a] = th_label (s_Setup, cx, 0, f, dim, a ? "Y  (UP)" : "X  (RIGHT)");
		lv_obj_set_width (s_AxT[a], cw);
		for (int k = 0; k < SXY_SLOTS; k++)
		{
			int y = lh + g + k * rh, n = a * SXY_SLOTS + k;
			s_SlotK[a][k] = th_key (s_Setup, cx, y, cw - vw - g, kh, "NONE", kind | TH_KEY_LEFT, slot_cb, (void *) (intptr_t) n);
			s_SlotV[a][k] = th_label (s_Setup, cx + cw - vw, y + (kh - lh) / 2, f, ink, "");
			lv_obj_set_width (s_SlotV[a][k], vw);
			lv_obj_set_style_text_align (s_SlotV[a][k], LV_TEXT_ALIGN_RIGHT, 0);
			s_SlotS[a][k] = th_slider (s_Setup, cx, y + kh + (o ? 2 : 4), cw, rh - kh - (o ? 4 : 10), 0, 200, depth_cb, (void *) (intptr_t) n);
		}
	}
	// the picker: the target's name, the kinds, the index and the param, SET / NONE / BACK
	s_Pick = th_box (pg, x0, top, aw, ah);
	s_PickT = th_label (s_Pick, 0, 0, o ? TH->f_body : TH->f_mid, ink, "");
	lv_obj_set_width (s_PickT, aw);
	const int ph1 = lv_font_get_line_height (o ? TH->f_body : TH->f_mid), sh = keyh * 2 + g;
	s_Kinds = th_seg (s_Pick, 0, ph1 + g, aw, sh, kKindMap, kind, kind_cb, 0);
	const int sk = o ? LU (26) : LU (56), ry = ph1 + sh + 2 * g, lw2 = o ? LU (40) : LU (90);
	const char *cap[2] = {"INDEX", "PARAM"};
	lv_obj_t **lab[2] = {&s_IdxL, &s_ParL}, **mk[2] = {&s_IdxM, &s_ParM}, **pk[2] = {&s_IdxP, &s_ParP};
	for (int r = 0; r < 2; r++)
	{
		int y = ry + r * (keyh + g);
		th_label (s_Pick, 0, y + (keyh - lh) / 2, f, dim, cap[r]);
		*mk[r] = th_key (s_Pick, lw2, y, sk, keyh, "-", kind, step_cb, (void *) (intptr_t) (2 * r));
		*lab[r] = th_label (s_Pick, lw2 + sk + g, y + (keyh - lh) / 2, f, ink, "");
		lv_obj_set_width (*lab[r], aw - lw2 - 2 * sk - 2 * g);
		lv_obj_set_style_text_align (*lab[r], LV_TEXT_ALIGN_CENTER, 0);
		*pk[r] = th_key (s_Pick, aw - sk, y, sk, keyh, "+", kind, step_cb, (void *) (intptr_t) (2 * r + 1));
	}
	const int by = ah - keyh, bw = (aw - 2 * g) / 3;
	s_PSet = th_key (s_Pick, 0, by, bw, keyh, "SET", o ? kind : TH_KEY_ACCENT, pick_end_cb, (void *) (intptr_t) 0);
	s_PNone = th_key (s_Pick, bw + g, by, bw, keyh, "NONE", kind, pick_end_cb, (void *) (intptr_t) 1);
	th_key (s_Pick, 2 * (bw + g), by, bw, keyh, "BACK", kind, pick_end_cb, (void *) (intptr_t) 2);
	show_mode ();
	refresh ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_xy_set_command (sidecar_xy_command_t fn) { s_Cmd = fn; }

void sidecar_xy_show (int on)
{
	if (!s_Page) return;
	if (!on && s_bDown) send_pos (), cmd ("xy=up"), s_bDown = 0, s_nTrail = 0, s_bPend = 0;	// the page left under the finger: released
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_xy_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

void sidecar_xy_setup (int on)
{
	if (!s_Page) return;
	s_bSetup = !!on, s_nPickA = -1;
	show_mode ();
	refresh ();
}
int sidecar_xy_setup_shown (void) { return s_Page && s_bSetup; }

void sidecar_xy_pick (int axis, int slot)
{
	if (!s_Page) return;
	if (axis < 0 || axis > 1 || slot < 0 || slot >= SXY_SLOTS) s_nPickA = -1;
	else
	{
		s_bSetup = 1, s_nPickA = axis, s_nPickS = slot;
		s_nKind = K_PERF, s_nIdx = 0, s_nPar = 0;
		if (s_V.s[axis][slot].name[0]) find_name (s_V.s[axis][slot].name, &s_nKind, &s_nIdx, &s_nPar);
		pick_refresh ();
	}
	show_mode ();
	refresh ();
}
int sidecar_xy_picking (void) { return s_nPickA < 0 ? -1 : s_nPickA * SXY_SLOTS + s_nPickS; }
void sidecar_xy_hold_key (void) { if (s_Page) hold_cb (0, 0, 0); }
int sidecar_xy_hold_shown (void) { return s_V.hold; }
const char *sidecar_xy_text (int which)
{
	lv_obj_t *l = !s_Page ? 0 : which == 0 ? s_PickT : which == 1 ? s_ParL : which - 2 < 2 * SXY_SLOTS ? s_Sum[which - 2] : 0;
	return l ? lv_label_get_text (l) : "";
}

void sidecar_xy_pad_area (int *x, int *y, int *w, int *h) { *x = s_PadX, *y = s_PadY, *w = s_PadW, *h = s_PadH; }

static void short_name (const char *n, char *b, int cap)	// the summary's: RYTM:PERF3 -> PERF3
{
	lv_snprintf (b, cap, "%s", !strncmp (n, "RYTM:", 5) ? n + 5 : n);
}
static void refresh (void)
{
	const sidecar_xy_view_t *v = &s_V;
	const int o = oled ();
	char t[48], n[16];
	if (s_TitleBar) th_titlebar_set (s_TitleBar, "XY PAD", v->hold ? "HOLD" : "", 0);
	else th_label_set (s_Title, v->hold ? "XY PAD  HOLD" : "XY PAD");
	th_key_check (s_Hold, v->hold);
	pos_text ();
	int line = 0;
	for (int a = 0; a < 2; a++)
		for (int k = 0; k < SXY_SLOTS; k++)
		{
			const sidecar_xy_slot_t *s = &v->s[a][k];
			if (s->name[0] && line < 2 * SXY_SLOTS)
			{
				short_name (s->label[0] ? s->label : s->name, n, sizeof n);
				lv_snprintf (t, sizeof t, "%c %s %+d", a ? 'Y' : 'X', n, s->depth);
				th_label_fit (s_Sum[line++], t);
			}
			th_key_text (s_SlotK[a][k], s->name[0] ? (s->label[0] ? s->label : s->name) : "NONE");
			lv_snprintf (t, sizeof t, s->name[0] ? "%+d" : "", s->depth);
			if (!th_slider_dragging (s_SlotS[a][k]))
			{
				th_label_set (s_SlotV[a][k], t);
				th_slider_set (s_SlotS[a][k], s->name[0] ? s->depth + 100 : 100);
			}
		}
	for (; line < 2 * SXY_SLOTS; line++) th_label_set (s_Sum[line], "");
	th_label_set (s_Info, !v->valid ? "WAITING FOR THE ENGINE"
		: v->locked ? (o ? "XY LOCKED: THE FILE IS FROM A NEWER BUILD, NOT SAVED" : "XY locked: the file is from a newer build, not saved")
		: s_nPickA >= 0 ? (o ? "PICK A KIND, THEN ITS INDEX AND PARAMETER; SET" : "Pick a kind, then its index and parameter; SET")
		: s_bSetup ? (o ? "TAP A SLOT: ITS TARGET. THE SLIDER: ITS DEPTH (- INVERTS)" : "Tap a slot to pick its target; the slider sets its depth (minus inverts)")
		: v->hold ? (o ? "HOLD ON: THE VALUES STAY WHERE YOU LIFT" : "HOLD on: the values stay where you lift your finger")
		: (o ? "DRAG ON THE PAD. LIFT: THE VALUES RETURN" : "Drag on the pad; lift and the values return"));
	if (s_nPickA >= 0) pick_refresh ();
	lv_obj_invalidate (s_Pad);
}

void sidecar_xy_set (const sidecar_xy_view_t *v)
{
	if (!s_Page) return;
	// the engine touching with no finger here: its xy=up lost (a full console queue); again after 200 ms, then each 500
	uint32_t now = lv_tick_get ();
	if (v->valid && v->touching && !s_bDown)
	{
		if (!s_nStuck) s_nStuck = now | 1;
		else if (now - s_nStuck >= 200 && (!s_nUpAt || now - s_nUpAt >= 500)) cmd ("xy=up"), s_nUpAt = now | 1;
	}
	else s_nStuck = s_nUpAt = 0;
	// a HOLD tap: shown as tapped until the engine's view has it, or for 300 ms (then the engine's: the command lost)
	int keep = s_nHoldAt && (int32_t) (now - s_nHoldAt) < 300, hold = s_V.hold;	// (signed: s_nHoldAt is now | 1)
	if (v->gen == s_nGen && (!s_nHoldAt || keep)) return;
	s_V = *v;
	if (keep && v->hold != hold) s_V.hold = hold;
	else s_nHoldAt = 0;
	s_nGen = v->gen;
	refresh ();
}
