// pi/ui/sidecar_status.c - see sidecar_status.h. Five panels of caption / value rows: labels (th_label_set redraws only
// a changed text) and horizontal meters (th_meter_set only a changed value), no code-drawn area, so a second's update
// of a page whose numbers barely move redraws a few labels. OLED: dotted group panels, captions in the micro face,
// values in the body face, the voices' load as dot ladders (teal over 60 %), MUTED and errors in the teal's tint.
// CLASSIC: display windows on the grey panel, the same rows in its fonts, the loads as recessed LED bars.
#include "sidecar_status.h"
#include "sidecar_theme.h"
#include "sidecar_vlabel.h"
#include "lvgl.h"
#include <string.h>

enum { R_RYTM, R_OB, R_USBC, R_NET, R_WEB, R_CARD, R_USB, R_NVME, R_UI, R_LINKS };
enum { V_SIDECAR, V_CIRCLE, V_LVGL, V_MOD, V_LIB, V_N };
enum { C_BLOCK, C_PASS, C_UNDER, C_FIFO, C_N };
static const char *const kLinkCap[R_LINKS] = {"RYTM", "OB AUDIO", "USB-C", "NETWORK", "WEB PAGE", "SD CARD", "USB DRIVE", "NVME SSD", "DISPLAY"};
static const char *const kLinkCapC[R_LINKS] = {"Rytm", "Overbridge", "USB-C", "Network", "Web page", "SD card", "USB drive", "NVMe SSD", "Display"};
static const char *const kVerCap[V_N] = {"SIDECAR", "CIRCLE", "LVGL", "RYTM MOD", "LIBRARY"};
static const char *const kVerCapC[V_N] = {"Sidecar", "Circle", "LVGL", "Rytm mod", "Library"};
static const char *const kCpuCap[C_N] = {"BLOCK MAX", "PASS MAX", "UNDERRUNS", "FIFO"};
static const char *const kCpuCapC[C_N] = {"Longest block", "Longest pass", "Underruns", "FIFO"};

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Sub;
static lv_obj_t *s_VMach[SST_VOICES], *s_VLoad[SST_VOICES], *s_VPct[SST_VOICES];
static lv_obj_t *s_Clock;				// the ARM clock, on the CPU panel's title line
static lv_obj_t *s_Temp;				// the temperature, right of the FIFO row: in the alert colour while the warning is on
static lv_obj_t *s_Inst;				// E0: the sequencer's instances, on the VOICES panel's title line
static lv_obj_t *s_Pair;				// the pairing code (pi/engine/paircode.h), on the LINKS panel's title line
static lv_obj_t *s_Core[SST_CORES], *s_CorePct[SST_CORES], *s_Cpu[C_N], *s_Fx[2], *s_FxPp[2], *s_Link[R_LINKS], *s_Ver[V_N];
static sidecar_status_view_t s_V;
static unsigned s_nGen = ~0u;

static int oled (void) { return TH->id == THEME_OLED; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_alert (void) { return oled () ? TH->accent_hi : TH->bad; }
static const lv_font_t *f_cap (void) { return oled () ? TH->f_micro : &lv_font_montserrat_12; }
static const lv_font_t *f_val (void) { return oled () ? TH->f_body : &lv_font_montserrat_14; }

static lv_obj_t *val (lv_obj_t *p, int x, int y, int w, int right)
{
	lv_obj_t *l = th_label (p, x, y, f_val (), c_ink (), "-");
	lv_obj_set_size (l, w, lv_font_get_line_height (f_val ()));
	lv_label_set_long_mode (l, LV_LABEL_LONG_MODE_DOTS);
	if (right) lv_obj_set_style_text_align (l, LV_TEXT_ALIGN_RIGHT, 0);
	return l;
}
static void set (lv_obj_t *l, const char *t, uint32_t rgb)
{
	if (!l) return;
	th_label_set (l, t);
	th_label_color (l, rgb);
}

// a panel of n caption / value rows from y (in the panel) at pitch rp; the values' labels into out
static void rows (lv_obj_t *p, int y, int rp, int w, int capw, int n, const char *const *cap, lv_obj_t **out)
{
	const int O = oled (), fc = lv_font_get_line_height (f_cap ()), fv = lv_font_get_line_height (f_val ());
	for (int i = 0; i < n; i++)
	{
		int ry = y + i * rp;
		th_label (p, O ? 6 : 12, ry + (fv - fc) / 2 + (O ? 1 : 1), f_cap (), O ? TH->dim : TH->win_dim, cap[i]);
		out[i] = val (p, (O ? 6 : 12) + capw, ry, w - capw - (O ? 10 : 24), 0);
	}
}

void sidecar_status_create (void *parent, int y0)
{
	const int O = oled (), dx = TH->dx, dy = TH->dy;
	const int pw = O ? TH->w - 48 : TH->w, ph = O ? TH->h : 428 + dy;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, pw, ph);
	s_Page = pg;
	s_nGen = ~0u;
	memset (&s_V, 0, sizeof s_V);
	const int kind = O ? TH_PANEL_GROUP : TH_PANEL_WINDOW;
	// two columns: VOICES, SEND FX, VERSIONS | CPU, LINKS. 1024x600: wider columns, taller rows
	const int top = O ? 15 : 6, gap = O ? 4 : 8, mx = O ? 0 : 8;
	const int cw = (pw - 2 * mx - gap) / 2, rx = mx + cw + gap, H = ph - top - (O ? 0 : 4);
	const int pt = O ? 12 : 26;				// the panel's title (its rows start below it)
	const int rp = O ? 12 + dy / 16 : 22 + dy / 16;	// a row
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, pw, 13);
		th_titlebar_set (s_TitleBar, "SIDECAR STATUS", "", 0);
		s_Title = s_Sub = 0;
	}
	else
	{
		s_TitleBar = 0;
		s_Title = s_Sub = 0;
	}
	// ---- left: VOICES (8 rows), SEND FX (2), VERSIONS (3)
	const int vh = pt + 8 * rp + (O ? 2 : 6), fh = pt + 2 * rp + (O ? 2 : 6), verh = H - vh - fh - 2 * gap;
	lv_obj_t *vp = th_panel (pg, mx, top, cw, vh, O ? "VOICES" : "Voices: machine and load", kind);	// (seqx: shorter, the instances right of it)
	const int namew = O ? 35 : 64, machw = O ? 50 + dx / 8 : 92 + dx / 8, pctw = O ? 26 : 52;
	for (int n = 0; n < SST_VOICES; n++)
	{
		int vi = kVoiceOrder[n], ry = pt + n * rp, fv = lv_font_get_line_height (f_val ());
		lv_obj_t *nm = th_label (vp, O ? 6 : 12, ry, f_val (), O ? TH->sub : TH->accent, kVoiceName[vi]);
		(void) nm;
		s_VMach[vi] = val (vp, (O ? 6 : 12) + namew, ry, machw, 0);
		int bx = (O ? 6 : 12) + namew + machw + (O ? 4 : 8), bw = cw - bx - pctw - (O ? 8 : 22);
		s_VLoad[vi] = th_meter (vp, bx, ry + (O ? 2 : 4), bw, O ? fv - 4 : fv - 6, 500, TH_METER_H, O ? TH->ink : TH->accent);	// full: half a core
		th_meter_set (s_VLoad[vi], 0);
		s_VPct[vi] = val (vp, cw - pctw - (O ? 6 : 18), ry, pctw, 1);
	}
	const int ix = O ? cw / 3 : cw / 2;		// E0: right of the title (the OLED's is short)
	s_Inst = th_label (vp, ix, O ? 1 : 7, f_cap (), O ? TH->dim : TH->win_dim, "");
	lv_obj_set_size (s_Inst, cw - ix - (O ? 8 : 18), lv_font_get_line_height (f_cap ()));
	lv_label_set_long_mode (s_Inst, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_align (s_Inst, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_t *fp = th_panel (pg, mx, top + vh + gap, cw, fh, O ? "SEND FX" : "Send FX (the kit's)", kind);
	for (int k = 0; k < 2; k++)
	{
		int ry = pt + k * rp;
		th_label (fp, O ? 6 : 12, ry, f_val (), k ? c_ink () : (O ? th_oled_secondary (127) : 0x34C85A), k ? "FX 2" : "FX 1");
		s_Fx[k] = val (fp, (O ? 6 : 12) + namew + (O ? 4 : 8), ry, cw - namew - (O ? 60 : 120), 0);
		s_FxPp[k] = val (fp, cw - (O ? 46 : 92), ry, O ? 40 : 74, 1);
	}
	lv_obj_t *verp = th_panel (pg, mx, top + vh + fh + 2 * gap, cw, verh, O ? "VERSIONS + LIBRARY" : "Versions and the sample library", kind);
	int vrp = (verh - pt - (O ? 2 : 6)) / V_N;		// (five rows: tighter where the panel is short)
	if (vrp > rp) vrp = rp;
	int vy = (verh - pt - V_N * vrp - (O ? 2 : 6)) / 2;
	rows (verp, pt + (vy > 0 ? vy : 0), vrp, cw, O ? 54 : 96, V_N, O ? kVerCap : kVerCapC, s_Ver);
	// ---- right: CPU (4 cores, 4 rows), LINKS (9 rows)
	const int cph = pt + 4 * rp + C_N * rp + (O ? 4 : 10);
	lv_obj_t *cp = th_panel (pg, rx, top, cw, cph, O ? "CPU" : "CPU", kind);
	for (int c = 0; c < SST_CORES; c++)
	{
		int ry = pt + c * rp, fv = lv_font_get_line_height (f_val ());
		char b[16];
		lv_snprintf (b, sizeof b, O ? "CORE %d" : "Core %d", c);
		th_label (cp, O ? 6 : 12, ry + (O ? 1 : 1), f_cap (), O ? TH->dim : TH->win_dim, b);
		int bx = O ? 50 : 76, bw = cw - bx - pctw - (O ? 8 : 22);
		s_Core[c] = th_meter (cp, bx, ry + (O ? 2 : 4), bw, O ? fv - 4 : fv - 6, 100, TH_METER_H, O ? TH->ink : TH->accent);
		th_meter_set (s_Core[c], -1);
		s_CorePct[c] = val (cp, cw - pctw - (O ? 6 : 18), ry, pctw, 1);
	}
	rows (cp, pt + 4 * rp + (O ? 3 : 6), rp, cw, O ? 64 : 120, C_N, O ? kCpuCap : kCpuCapC, s_Cpu);
	// like the VOICES title's instances; from a quarter in (right aligned): room for CLOCK LOWERED
	s_Clock = th_label (cp, cw / 4, O ? 1 : 7, f_cap (), O ? TH->dim : TH->win_dim, "");
	lv_obj_set_size (s_Clock, cw * 3 / 4 - (O ? 8 : 18), lv_font_get_line_height (f_cap ()));
	lv_obj_set_style_text_align (s_Clock, LV_TEXT_ALIGN_RIGHT, 0);
	{	// the temperature shares the FIFO row (a row more would squeeze LINKS): the FIFO's value gets the left part
		const int tw = O ? 56 : 130, fx = (O ? 6 : 12) + (O ? 64 : 120);
		lv_obj_set_width (s_Cpu[C_FIFO], cw - fx - tw - (O ? 10 : 24));
		s_Temp = val (cp, cw - tw - (O ? 6 : 18), pt + 4 * rp + (O ? 3 : 6) + C_FIFO * rp, tw, 1);
	}
	const int lh = H - cph - gap;
	lv_obj_t *lp = th_panel (pg, rx, top + cph + gap, cw, lh, O ? "LINKS" : "Links", kind);
	const int lrp = (lh - pt - (O ? 3 : 8)) / R_LINKS;
	rows (lp, pt + (O ? 1 : 2), lrp, cw, O ? 54 : 104, R_LINKS, O ? kLinkCap : kLinkCapC, s_Link);
	// the pairing code right of the LINKS title (as E0's instances sit on the VOICES title): the panel's rows are
	// full at this height, and the code belongs with the network anyway
	const int px = O ? cw / 3 : cw / 2;
	s_Pair = th_label (lp, px, O ? 1 : 7, f_cap (), O ? TH->dim : TH->win_dim, "");
	lv_obj_set_size (s_Pair, cw - px - (O ? 8 : 18), lv_font_get_line_height (f_cap ()));
	lv_label_set_long_mode (s_Pair, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_text_align (s_Pair, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_status_show (int on)
{
	if (!s_Page) return;
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
}

int sidecar_status_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

static void mb_text (char *b, int cap, unsigned mb)
{
	if (mb >= 10240) lv_snprintf (b, cap, "%u GB", mb / 1024);
	else if (mb >= 1024) lv_snprintf (b, cap, "%u.%u GB", mb / 1024, mb % 1024 * 10 / 1024);
	else lv_snprintf (b, cap, "%u MB", mb);
}

// a USB / NVMe volume's row: "57/59 GB EXFAT USB3" (OLED: its narrow column) or "57 GB free of 59 GB, exFAT, USB 3"
static void vol_text (char *b, int cap, int state, const char *fs, unsigned free_mb, unsigned total_mb, const char *link, int more)
{
	const int O = oled ();
	if (state == 2 && free_mb == ~0u)
	{
		char t[16];
		mb_text (t, sizeof t, total_mb);
		lv_snprintf (b, cap, O ? "%s %s, FREE ?" : "%s, %s, free space not counted", t, fs);
	}
	else if (state == 2)
	{
		char f[16], t[16], l[8];
		mb_text (f, sizeof f, free_mb);
		mb_text (t, sizeof t, total_mb);
		if (O)
		{
			int nf = (int) strlen (f), nt = (int) strlen (t);	// the same unit: once ("57/59 GB")
			if (nf > 3 && nt > 3 && !strcmp (f + nf - 3, t + nt - 3)) f[nf - 3] = 0;
			int k = 0;
			for (const char *c = link; *c && k < (int) sizeof l - 1; c++) if (*c != ' ') l[k++] = *c;	// "USB3"
			l[k] = 0;
			lv_snprintf (b, cap, "%s/%s %s%s%s", f, t, fs, l[0] ? " " : "", l);
		}
		else lv_snprintf (b, cap, "%s free of %s, %s%s%s", f, t, fs, link[0] ? ", " : "", link);
		if (more) lv_snprintf (b + strlen (b), cap - (int) strlen (b), " +%d", more);
	}
	else if (state == 1) lv_snprintf (b, cap, O ? "NOT MOUNTED" : "not mounted (format? see stor)");
	else if (state == 3) lv_snprintf (b, cap, O ? "EJECTED: SAFE TO PULL" : "ejected: safe to remove");
	else if (state == 4) lv_snprintf (b, cap, O ? "-" : "not on this board");
	else lv_snprintf (b, cap, O ? "NONE" : "none");
}

// the library's row: "48213 FILES 61%" / "1203 FILES  2 DUP  1 MISS" (OLED), "48213 files, 61 % hashed, 120 pending,
// waiting: playing" / "1203 files, all hashed, 2 duplicates, 1 missing" (CLASSIC)
static void lib_text (char *b, int cap, const sidecar_status_view_t *v)
{
	const int O = oled ();
	if (v->lib_state == 0) { lv_snprintf (b, cap, O ? "OFF" : "off (no card)"); return; }
	if (v->lib_state == 5) { lv_snprintf (b, cap, O ? "FAILED" : "failed (see the log)"); return; }
	if (v->lib_state == 1) { lv_snprintf (b, cap, O ? "LOADING" : "loading the index"); return; }
	if (v->lib_state == 2) { lv_snprintf (b, cap, O ? "%u FILES, WALKING" : "%u files, looking at the card", v->lib_files); return; }
	// short enough for its row (UX review R1): OLED 18 characters (thousands as K), CLASSIC about 38
	char f[16];
	if (O && v->lib_files >= 10000) lv_snprintf (f, sizeof f, "%uK", v->lib_files / 1000);
	else lv_snprintf (f, sizeof f, "%u", v->lib_files);
	int n;
	if (v->lib_state == 3)
		n = lv_snprintf (b, cap, O ? "%s FILES %u%%" : "%s files, %u %%, %u to go", f, v->lib_pct, v->lib_pending);
	else if (O && v->lib_dups && v->lib_missing)
		n = lv_snprintf (b, cap, "%s %u DUP %u MISS", f, v->lib_dups, v->lib_missing);
	else
	{
		n = lv_snprintf (b, cap, O ? "%s FILES" : "%s files", f);
		if (v->lib_dups && n < cap) n += lv_snprintf (b + n, cap - n, O ? " %u DUP" : ", %u dup.", v->lib_dups);
		if (v->lib_missing && n < cap) n += lv_snprintf (b + n, cap - n, O ? " %u MISS" : ", %u missing", v->lib_missing);
	}
	if (v->lib_waiting[0] && n < cap && !O)
	{
		char w[16];
		int k = 0;
		for (const char *c = v->lib_waiting; *c && k < 15; c++) w[k++] = *c >= 'A' && *c <= 'Z' ? *c + 32 : *c;
		w[k] = 0;
		lv_snprintf (b + n, cap - n, " (%s)", w);
	}
}

void sidecar_status_set (const sidecar_status_view_t *v)
{
	if (!s_Page || v->gen == s_nGen) return;
	s_V = *v;
	s_nGen = v->gen;
	const int O = oled ();
	char b[96];
	uint32_t ink = c_ink (), dim = c_dim (), al = c_alert ();
	if (O)
	{
		lv_snprintf (b, sizeof b, "%s", v->build[0] ? v->build : "");
		char *sp = strchr (b, ' ');			// "0.2.0 (abc1234) ..." -> "0.2.0"
		if (sp) *sp = 0;
		th_titlebar_set (s_TitleBar, "SIDECAR STATUS", b, 0);
		th_titlebar_led (s_TitleBar, v->rytm ? LED_OK : 0);
	}
	// voices
	for (int vi = 0; vi < SST_VOICES; vi++)
	{
		const char *m = v->mach[vi][0] ? v->mach[vi] : "-";
		if (v->muted[vi]) { lv_snprintf (b, sizeof b, "%s", O ? "MUTED" : "MUTED"); set (s_VMach[vi], b, al); }
		else set (s_VMach[vi], m, ink);
		int l = v->vload[vi];
		th_meter_set (s_VLoad[vi], l < 0 ? -1 : l > 500 ? 500 : l);
		if (l < 0) lv_snprintf (b, sizeof b, "-");
		else if (l < 100) lv_snprintf (b, sizeof b, "%d.%d%%", l / 10, l % 10);
		else lv_snprintf (b, sizeof b, "%d%%", (l + 5) / 10);
		set (s_VPct[vi], b, l > 600 ? al : l < 0 ? dim : ink);
	}
	// send FX
	for (int k = 0; k < 2; k++)
	{
		int off = !v->fx_type[k][0] || !strcmp (v->fx_type[k], "OFF");
		set (s_Fx[k], v->fx_type[k][0] ? v->fx_type[k] : "-", off ? dim : ink);
		set (s_FxPp[k], off ? "" : v->fx_post[k] ? "POST" : "PRE", dim);
	}
	// cpu
	for (int c = 0; c < SST_CORES; c++)
	{
		int l = v->load[c];
		th_meter_set (s_Core[c], l < 0 ? -1 : l > 100 ? 100 : l);
		if (l < 0) lv_snprintf (b, sizeof b, O ? "N/M" : "n/m"); else lv_snprintf (b, sizeof b, "%d%%", l);
		set (s_CorePct[c], b, l < 0 ? dim : l > 80 ? al : ink);
	}
	if (v->valid) lv_snprintf (b, sizeof b, O ? "%u US / %u" : "%u us of %u", v->block_max_us, v->block_budget_us); else lv_snprintf (b, sizeof b, "-");
	set (s_Cpu[C_BLOCK], b, v->block_budget_us && v->block_max_us > v->block_budget_us * 8 ? al : ink);
	if (v->valid) lv_snprintf (b, sizeof b, O ? "%u US" : "%u us", v->pass_max_us); else lv_snprintf (b, sizeof b, "-");
	set (s_Cpu[C_PASS], b, v->pass_max_us > 2000 ? al : ink);
	lv_snprintf (b, sizeof b, "%u", v->underruns);
	set (s_Cpu[C_UNDER], b, v->underruns ? al : ink);
	if (v->fifo_target) lv_snprintf (b, sizeof b, O ? "%u/%u" : "min %u of %u", v->fifo_min, v->fifo_target);
	else lv_snprintf (b, sizeof b, "-");
	set (s_Cpu[C_FIFO], b, ink);
	if (v->temp_c <= 0) lv_snprintf (b, sizeof b, "-");	// (the warning's own state, not the number, picks the colour)
	else if (v->temp_warn) lv_snprintf (b, sizeof b, O ? "%dC HOT" : "%d \xC2\xB0" "C  hot", v->temp_c);
	else lv_snprintf (b, sizeof b, O ? "%dC" : "%d \xC2\xB0" "C", v->temp_c);
	set (s_Temp, b, v->temp_warn ? al : v->temp_c > 0 ? ink : dim);
	// the clock Circle's throttle lowered (thermal.h clock_low_mhz): said on the title line - in the alert colour with the
	// warning, quietly (no banner, no popup) between the throttle's 78 C and the warning's 80
	// (OLED: the title line holds 13 characters - the banner says the MHz while warned)
	if (v->clock_low_mhz > 0 && O) lv_snprintf (b, sizeof b, "CLOCK LOWERED");
	else if (v->clock_low_mhz > 0) lv_snprintf (b, sizeof b, "%d MHz: clock lowered (hot)", v->clock_low_mhz);
	else if (v->cpu_mhz > 0) lv_snprintf (b, sizeof b, O ? "%d MHZ" : "%d MHz", v->cpu_mhz); else b[0] = 0;
	set (s_Clock, b, v->clock_low_mhz > 0 ? (v->temp_warn ? al : ink) : dim);
	// E0: the sequencer's instances: in use of the pool, one's measured cost (% of a core) and RAM
	if (!v->inst_total) b[0] = 0;
	else if (v->inst_cpu100 < 0) lv_snprintf (b, sizeof b, O ? "INST %d/%d %uK" : "Instances %d/%d, %u KB", v->inst_used, v->inst_total, v->inst_kb);
	else lv_snprintf (b, sizeof b, O ? "INST %d/%d %d.%02d%% %uK" : "Instances %d/%d, %d.%02d%%, %u KB", v->inst_used, v->inst_total,
			  v->inst_cpu100 / 100, v->inst_cpu100 % 100, v->inst_kb);
	set (s_Inst, b, v->inst_total && v->inst_used >= v->inst_total ? al : dim);
	// links
	set (s_Link[R_RYTM], v->rytm ? (O ? "CONNECTED" : "connected") : (O ? "NOT CONNECTED" : "not connected"), v->rytm ? ink : al);
	if (v->ob) lv_snprintf (b, sizeof b, O ? "RUNNING" : "running, %u blocks", v->blocks);
	else lv_snprintf (b, sizeof b, O ? "STOPPED" : "stopped");
	set (s_Link[R_OB], b, v->ob ? ink : dim);
	if (v->gadget[0]) lv_snprintf (b, sizeof b, "%s%s", v->gadget, v->mac < 0 ? "" : v->mac ? (O ? " AUDIO" : ", computer audio on") : (O ? "" : ", computer audio off"));
	else lv_snprintf (b, sizeof b, O ? "OFF" : "off");
	set (s_Link[R_USBC], b, v->gadget[0] ? ink : dim);
	const char *kind = v->wifi ? (O ? "WI-FI" : "Wi-Fi") : (O ? "ETH" : "Ethernet");
	if (v->net == 2) lv_snprintf (b, sizeof b, "%s  %s", kind, v->ip[0] ? v->ip : "-");
	else if (v->net == 1) lv_snprintf (b, sizeof b, O ? "%s DOWN" : "%s down", kind);
	else lv_snprintf (b, sizeof b, O ? "NONE" : "none");
	set (s_Link[R_NET], b, v->net == 2 ? ink : v->net == 1 ? al : dim);
	if (O) lv_snprintf (b, sizeof b, "%s", v->host[0] ? v->host : "sidecar.local");
	else lv_snprintf (b, sizeof b, "http://%s/  (link %s)", v->host[0] ? v->host : "sidecar.local", v->link[0] ? v->link : "-");
	set (s_Link[R_WEB], b, ink);
	// the pairing code (pi/engine/paircode.h): the gate off, or a fixed code, in the alert colour - a dev card must
	// never be mistaken for a secured one
	lv_snprintf (b, sizeof b, O ? "PAIR %s" : "pair code %s", v->pair[0] ? v->pair : (O ? "?" : "unknown"));
	set (s_Pair, b, v->pair_warn ? al : v->pair[0] ? c_ink () : dim);
	if (v->card_total_mb)
	{
		char t[16];
		mb_text (b, 24, v->card_free_mb);
		mb_text (t, sizeof t, v->card_total_mb);
		lv_snprintf (b + strlen (b), sizeof b - strlen (b), O ? " / %s" : " free of %s", t);
	}
	else lv_snprintf (b, sizeof b, O ? "NO CARD" : "no card");
	set (s_Link[R_CARD], b, v->card_total_mb && v->card_free_mb < 1024 ? al : v->card_total_mb ? ink : dim);
	vol_text (b, sizeof b, v->usb_state, v->usb_fs, v->usb_free_mb, v->usb_total_mb, v->usb_link, v->usb_more);
	set (s_Link[R_USB], b, v->usb_state == 2 && v->usb_free_mb < 1024 && v->usb_free_mb != ~0u ? al : v->usb_state == 2 ? ink : v->usb_state == 1 ? al : dim);
	vol_text (b, sizeof b, v->nvme_state, v->nvme_fs, v->nvme_free_mb, v->nvme_total_mb, "", 0);
	set (s_Link[R_NVME], b, v->nvme_state == 2 && v->nvme_free_mb < 1024 && v->nvme_free_mb != ~0u ? al : v->nvme_state == 2 ? ink : v->nvme_state == 1 ? al : dim);
	lv_snprintf (b, sizeof b, O ? "UPDATE %u.%u MS" : "longest update %u.%u ms", v->ui_max_us / 1000, v->ui_max_us % 1000 / 100);
	set (s_Link[R_UI], b, v->ui_max_us > 16000 ? al : ink);
	// versions (OLED: the build without its date)
	lv_snprintf (b, sizeof b, "%s", v->build[0] ? v->build : "-");
	if (O) { char *p = strchr (b, ')'); if (p) p[1] = 0; }
	set (s_Ver[V_SIDECAR], b, ink);
	const char *cv = strncmp (v->circle, "Circle ", 7) ? v->circle : v->circle + 7;
	set (s_Ver[V_CIRCLE], cv[0] ? cv : "-", ink);
	lv_snprintf (b, sizeof b, "%d.%d.%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
	set (s_Ver[V_LVGL], b, ink);
	set (s_Ver[V_MOD], v->rytm_mod[0] ? v->rytm_mod : (O ? "NONE SEEN (STOCK?)" : "none seen (stock?)"), v->rytm_mod[0] ? ink : dim);
	lib_text (b, sizeof b, v);
	set (s_Ver[V_LIB], b, v->lib_state == 5 || v->lib_missing ? al : v->lib_state == 0 || v->lib_waiting[0] ? dim : ink);
}
