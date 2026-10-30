// pi/ui/sidecar_project.c - see sidecar_project.h. Labels and keys only (no code-drawn area): a view that changes a
// status number redraws that label (th_label_set skips an unchanged text), the progress bar is one meter, a list
// change redraws its rows. OLED: the dotted look, captions in the micro face, the selection inverted, the FUNC teal for
// the destination picked; CLASSIC: display windows on the grey panel, rubber keys.
#include "sidecar_project.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include <stdint.h>
#include <string.h>

#define NROWS_MAX	12
#define NDLG		7

static lv_obj_t *s_Page, *s_TitleBar, *s_Title, *s_Dest, *s_Status, *s_Eject, *s_ListPanel, *s_Empty;
static lv_obj_t *s_Row[NROWS_MAX], *s_RowName[NROWS_MAX], *s_RowSize[NROWS_MAX], *s_RowWhen[NROWS_MAX];
static lv_obj_t *s_Info[6], *s_Key[6];
static lv_obj_t *s_Scrim, *s_Dlg, *s_DlgText[NDLG], *s_DlgBar, *s_DlgMach[4], *s_DlgNo, *s_DlgYes;
static sidecar_project_cmd_t s_Cmd;
static sidecar_project_view_t s_V;
static int s_bHave, s_nRows, s_nListing, s_nSel, s_nBSel, s_nTSel, s_nTop, s_nDest, s_nDlg, s_nDelete = -1, s_nVolsSeen = -1;
static char s_DestDrive[8];
static char s_VolsKey[64] = "?";	// the sticks' names as the picker shows them
static unsigned s_nEjectSeen;		// the eject answer last shown
static int s_bEjecting;
static unsigned s_nGenSeen = ~0u, s_nSeqSeen = ~0u;
static uint32_t s_nDelAt;
static int s_MachOff[SPJ_MACH];		// the summary's machines unticked
static char s_DestMap[SPJ_VOLS + 2][12];
static const char *s_DestPtr[SPJ_VOLS + 2];

enum { D_NONE, D_PROGRESS, D_SUMMARY, D_DONE, D_FAILED, D_CANCELLED, D_DELETE };
enum { K_EXPORT, K_IMPORT, K_DELETE, K_UP, K_DOWN, K_TRASH };
enum { L_PROJECTS, L_BUNDLES, L_TRASH };

static int oled (void) { return TH->id == THEME_OLED; }
static int W (void) { return oled () ? TH->w - 48 : TH->w; }
static int H (void) { return oled () ? TH->h : 428 + TH->dy; }
static uint32_t c_ink (void) { return oled () ? TH->ink : TH->win_ink; }
static uint32_t c_dim (void) { return oled () ? TH->dim : TH->win_dim; }
static uint32_t c_alert (void) { return oled () ? TH->accent_hi : TH->bad; }
static const lv_font_t *f_val (void) { return oled () ? TH->f_body : &lv_font_montserrat_18; }
static const lv_font_t *f_cap (void) { return oled () ? TH->f_micro : &lv_font_montserrat_14; }
static void cmd (const char *c) { if (s_Cmd) s_Cmd (c); }

static void upper (char *s) { if (oled ()) for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
static void set (lv_obj_t *l, const char *t, uint32_t rgb)
{
	char b[320];
	if (!l) return;
	lv_snprintf (b, sizeof b, "%s", t);
	upper (b);
	th_label_set (l, b);
	th_label_color (l, rgb);
}
static void set_name (lv_obj_t *l, const char *t, uint32_t rgb)	// a name, a path or a date: cut in the middle to fit
{
	char b[320];
	if (!l) return;
	lv_snprintf (b, sizeof b, "%s", t);
	upper (b);
	th_label_fit (l, b);
	th_label_color (l, rgb);
}
static lv_obj_t *label_w (lv_obj_t *p, int x, int y, int w, const lv_font_t *f, uint32_t rgb, int align)
{
	lv_obj_t *l = th_label (p, x, y, f, rgb, "");
	lv_obj_set_size (l, w, lv_font_get_line_height (f));	// one line: a longer text ends in dots
	lv_label_set_long_mode (l, LV_LABEL_LONG_MODE_DOTS);
	if (align > 0) lv_obj_set_style_text_align (l, LV_TEXT_ALIGN_RIGHT, 0);
	else if (align < 0) lv_obj_set_style_text_align (l, LV_TEXT_ALIGN_CENTER, 0);
	return l;
}
static void size_text (char *b, int n, unsigned long long v)
{
	if (v >= (10ull << 30)) lv_snprintf (b, n, "%u GB", (unsigned) (v >> 30));
	else if (v >= (1ull << 30)) lv_snprintf (b, n, "%u.%u GB", (unsigned) (v >> 30), (unsigned) ((v & ((1u << 30) - 1)) * 10 >> 30));
	else if (v >= (1u << 20)) lv_snprintf (b, n, "%u MB", (unsigned) (v >> 20));
	else lv_snprintf (b, n, "%u KB", (unsigned) ((v + 1023) >> 10));
}
static int running (int p) { return !(p == SPJ_IDLE || p == SPJ_DONE || p == SPJ_FAILED || p == SPJ_CANCELLED || p == SPJ_READY); }
static int src_ok (void) { return !strcmp (s_V.src, s_nDest ? s_V.vol[s_nDest - 1].drive : ""); }	// the bundles are the picker's
static int count (void)
{
	if (s_nListing == L_TRASH) return s_V.ntrash;
	return s_nListing ? (src_ok () && !s_V.reading ? s_V.nbund : 0) : s_V.nproj;
}
static int *sel (void) { return s_nListing == L_TRASH ? &s_nTSel : s_nListing ? &s_nBSel : &s_nSel; }
static const char *dest_token (void) { return s_nDest ? s_V.vol[s_nDest - 1].name : "card"; }
static void ask_lists (void)
{
	char b[48];
	lv_snprintf (b, sizeof b, "bundle=list:%s", dest_token ());
	cmd (b);
}

// ---- the list
static void scroll_to_sel (void)
{
	int n = count (), *s = sel ();
	if (*s >= n) *s = n - 1;
	if (*s < 0) *s = 0;
	if (*s < s_nTop) s_nTop = *s;
	if (*s >= s_nTop + s_nRows) s_nTop = *s - s_nRows + 1;
	if (s_nTop < 0) s_nTop = 0;
}
static void rows_update (void)
{
	const int O = oled ();
	scroll_to_sel ();
	int n = count ();
	char b[128], z[24];
	for (int r = 0; r < s_nRows; r++)
	{
		int i = s_nTop + r, on = i < n, cur = on && i == *sel ();
		if (on) lv_obj_remove_flag (s_Row[r], LV_OBJ_FLAG_HIDDEN); else { lv_obj_add_flag (s_Row[r], LV_OBJ_FLAG_HIDDEN); continue; }
		lv_obj_set_style_bg_opa (s_Row[r], cur ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		uint32_t ink = cur ? (O ? TH->sel_ink : TH->win_ink) : c_ink (), dim = cur ? (O ? TH->sel_ink : TH->win_ink) : c_dim ();
		if (s_nListing == L_TRASH)
		{
			const spj_trash_t *t = &s_V.trash[i];
			set_name (s_RowName[r], t->name[0] ? t->name : t->key, ink);
			set (s_RowSize[r], t->why, dim);
			if (strlen (t->date) >= 10) lv_snprintf (b, sizeof b, O ? "%.5s" : "%.10s", O ? t->date + 5 : t->date);
			else lv_snprintf (b, sizeof b, "#%u", t->id);
			set (s_RowWhen[r], b, dim);
		}
		else if (!s_nListing)
		{
			const spj_project_t *p = &s_V.proj[i];
			lv_snprintf (b, sizeof b, "%s%s%s", p->rytm, p->current ? " *" : "", p->imported ? (O ? " IMP." : "  imp.") : "");
			set_name (s_RowName[r], b, ink);
			size_text (z, sizeof z, p->bytes);
			set (s_RowSize[r], z, dim);
			if (p->date[0]) lv_snprintf (b, sizeof b, O ? "%.5s" : "%.10s", O ? p->date + 5 : p->date);
			else if (p->used) lv_snprintf (b, sizeof b, "#%u", p->used);
			else lv_snprintf (b, sizeof b, "-");
			set (s_RowWhen[r], b, dim);
		}
		else
		{
			const spj_bundle_t *u = &s_V.bund[i];
			lv_snprintf (b, sizeof b, "%s", u->name);
			char *e = strstr (b, ".sidecar.zip");
			if (e) *e = 0;
			if (u->incomplete) strcat (b, O ? " (PART)" : "  (incomplete)");
			set_name (s_RowName[r], b, u->incomplete ? dim : ink);
			size_text (z, sizeof z, u->size);
			set (s_RowSize[r], z, dim);
			if (O && strlen (u->date) >= 10) lv_snprintf (b, sizeof b, "%.5s", u->date + 5);	// OLED: "10-22" as the projects'
			else lv_snprintf (b, sizeof b, "%s", u->date);
			set_name (s_RowWhen[r], b, dim);
		}
	}
	if (s_V.reading || (s_nListing && !src_ok ())) set (s_Empty, O ? "READING..." : "Reading...", c_dim ());
	else if (!n && s_nListing == L_TRASH) set (s_Empty, O ? "THE TRASH IS EMPTY" : "The trash is empty", c_dim ());
	else if (!n) set (s_Empty, s_nListing ? (s_nDest ? "No bundles on the stick (/SIDECAR or its root)" : "No bundles in /Bundles on the card")
						  : "No projects on the card yet", c_dim ());
	else set (s_Empty, "", c_dim ());
}
static void info_update (void)
{
	const int O = oled ();
	char b[200], z[24];
	for (int k = 0; k < 6; k++) set (s_Info[k], "", c_ink ());
	if (s_nListing == L_TRASH)
	{
		if (s_nTSel >= count ()) return;
		const spj_trash_t *t = &s_V.trash[s_nTSel];
		set_name (s_Info[0], t->name[0] ? t->name : t->key, c_ink ());
		lv_snprintf (b, sizeof b, O ? "WAS %s  (%s)" : "was %s  (%s)", t->key, t->why);
		set (s_Info[1], b, c_dim ());
		lv_snprintf (b, sizeof b, O ? "%d ITEMS, ENTRY %06u" : "%d items, entry %06u", t->items, t->id);
		set (s_Info[2], b, c_ink ());
		if (t->date[0]) { lv_snprintf (b, sizeof b, O ? "MOVED %.10s %.5s" : "moved here %.10s %.5s", t->date, t->date + 11); set (s_Info[3], b, c_dim ()); }
		set (s_Info[4], O ? "RESTORE: TO ITS SLOT" : "Restore: back under its slot", c_dim ());
		set (s_Info[5], O ? "NEVER OVER FILES" : "(never over files or the current one)", c_dim ());
		return;
	}
	if (s_nListing)
	{
		if (s_nBSel >= count ()) return;
		const spj_bundle_t *u = &s_V.bund[s_nBSel];
		set_name (s_Info[0], u->name, c_ink ());
		size_text (z, sizeof z, u->size);
		lv_snprintf (b, sizeof b, O ? "%s  %s" : "%s, %s", z, u->date);
		set (s_Info[1], b, c_dim ());
		set_name (s_Info[2], u->path, c_dim ());
		set (s_Info[3], u->incomplete ? (O ? "INCOMPLETE: EXPORT AGAIN" : "Incomplete: export it again.")
					      : (O ? "IMPORT: CHECKS FIRST," : "IMPORT IT checks it all first,"),
		     u->incomplete ? c_alert () : c_dim ());
		set (s_Info[4], u->incomplete ? "" : O ? "THEN ASKS YOU" : "writes only after you confirm.", c_dim ());
		return;
	}
	if (s_nSel >= s_V.nproj) return;
	const spj_project_t *p = &s_V.proj[s_nSel];
	lv_snprintf (b, sizeof b, "%s%s", p->rytm, p->current ? (O ? " (CURRENT)" : "  (current)") : "");
	set_name (s_Info[0], b, c_ink ());
	if (strcmp (p->key, p->rytm)) { lv_snprintf (b, sizeof b, O ? "CARD NAME %s" : "card name: %s", p->key); set_name (s_Info[1], b, c_dim ()); }
	size_text (z, sizeof z, p->bytes);
	lv_snprintf (b, sizeof b, O ? "%u SIDECAR FILES, %s" : "%u Sidecar files, %s", p->files, z);
	set (s_Info[2], b, c_ink ());
	if (p->date[0]) lv_snprintf (b, sizeof b, O ? "USED %.10s %.5s" : "last used %.10s %.5s", p->date, p->date + 11);
	else if (p->used) lv_snprintf (b, sizeof b, O ? "LAST USED #%u" : "last used: #%u (no clock)", p->used);
	else lv_snprintf (b, sizeof b, O ? "NOT USED HERE YET" : "not used on this Sidecar yet");
	set (s_Info[3], b, c_dim ());
	set (s_Info[4], p->imported ? (O ? "RYTM: SAVE AS THIS" : "Imported: save the Rytm's as this")
				    : (O ? "+ ITS SAMPLES, SONGS" : "Bundle: + samples, songs, machines"),
	     p->imported ? c_alert () : c_dim ());
	set (s_Info[5], p->current ? (O ? "IN USE: NO DELETE" : "In use on the Rytm: no delete") : "", c_dim ());
}
static void keys_update (void)
{
	const int O = oled ();
	char b[32];
	int busy = running (s_V.phase) || s_V.phase == SPJ_READY, n = count ();
	lv_snprintf (b, sizeof b, O ? "TRASH (%d)" : "Trash (%d)", s_V.ntrash);
	th_key_text (s_Key[K_TRASH], b);
	if (s_nListing == L_PROJECTS) lv_obj_remove_flag (s_Key[K_TRASH], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Key[K_TRASH], LV_OBJ_FLAG_HIDDEN);
	th_key_enable (s_Key[K_TRASH], !busy);
	if (s_nListing == L_TRASH)
	{
		th_key_text (s_Key[K_EXPORT], O ? "RESTORE" : "Restore");
		th_key_text (s_Key[K_IMPORT], O ? "BACK" : "Back");
		lv_obj_add_flag (s_Key[K_DELETE], LV_OBJ_FLAG_HIDDEN);
		th_key_enable (s_Key[K_EXPORT], !busy && s_nTSel < n);
		th_key_enable (s_Key[K_IMPORT], 1);
	}
	else if (!s_nListing)
	{
		lv_snprintf (b, sizeof b, O ? "EXPORT %s" : "Export > %s", s_nDest ? s_V.vol[s_nDest - 1].name : (O ? "CARD" : "card"));
		th_key_text (s_Key[K_EXPORT], b);
		th_key_text (s_Key[K_IMPORT], O ? "IMPORT..." : "Import...");
		th_key_text (s_Key[K_DELETE], O ? "DELETE" : "Delete");
		lv_obj_remove_flag (s_Key[K_DELETE], LV_OBJ_FLAG_HIDDEN);
		th_key_enable (s_Key[K_EXPORT], !busy && n > 0);
		th_key_enable (s_Key[K_IMPORT], !busy);
		th_key_enable (s_Key[K_DELETE], !busy && n > 0 && s_nSel < n && !s_V.proj[s_nSel].current);
	}
	else
	{
		const spj_bundle_t *u = s_nBSel < count () ? &s_V.bund[s_nBSel] : 0;
		th_key_text (s_Key[K_EXPORT], O ? "IMPORT IT" : "Import it");
		th_key_text (s_Key[K_IMPORT], O ? "BACK" : "Back");
		lv_obj_add_flag (s_Key[K_DELETE], LV_OBJ_FLAG_HIDDEN);
		th_key_enable (s_Key[K_EXPORT], !busy && u && !u->incomplete);
		th_key_enable (s_Key[K_IMPORT], 1);
	}
	th_key_enable (s_Key[K_UP], s_nTop > 0);
	th_key_enable (s_Key[K_DOWN], s_nTop + s_nRows < n);
}
static void top_update (void)
{
	const int O = oled ();
	char b[96], f[24];
	// the picker: CARD and each stick
	char vk[64] = "";
	for (int i = 0; i < s_V.nvol; i++) { size_t l = strlen (vk); lv_snprintf (vk + l, sizeof vk - l, "%s,", s_V.vol[i].drive); }
	if (s_V.nvol != s_nVolsSeen || strcmp (vk, s_VolsKey))	// (a stick swapped for another: its name)
	{
		s_nVolsSeen = s_V.nvol;
		lv_snprintf (s_VolsKey, sizeof s_VolsKey, "%s", vk);
		int k = 0;
		lv_snprintf (s_DestMap[k], sizeof s_DestMap[0], "%s", O ? "CARD" : "SD card"), s_DestPtr[k] = s_DestMap[k], k++;
		for (int i = 0; i < s_V.nvol && i < SPJ_VOLS; i++)
			lv_snprintf (s_DestMap[k], sizeof s_DestMap[0], "%s", s_V.vol[i].name), s_DestPtr[k] = s_DestMap[k], k++;
		s_DestMap[k][0] = 0, s_DestPtr[k] = s_DestMap[k];
		th_seg_map (s_Dest, s_DestPtr);
	}
	// the destination picked: kept by its drive while sticks come and go
	if (s_nDest)
	{
		int at = 0;
		for (int i = 0; i < s_V.nvol; i++) if (!strcmp (s_V.vol[i].drive, s_DestDrive)) at = i + 1;
		s_nDest = at;
		if (!at) s_DestDrive[0] = 0;
	}
	th_seg_set (s_Dest, s_nDest);
	if (s_nDest) lv_obj_remove_flag (s_Eject, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Eject, LV_OBJ_FLAG_HIDDEN);
	// free space: the card and the sticks
	b[0] = 0;
	if (s_V.card_free) { size_text (f, sizeof f, s_V.card_free); lv_snprintf (b, sizeof b, O ? "CARD %s" : "SD card %s free", f); }
	for (int i = 0; i < s_V.nvol; i++)
	{
		size_t l = strlen (b);
		if (s_V.vol[i].free_known) size_text (f, sizeof f, s_V.vol[i].free); else lv_snprintf (f, sizeof f, "?");
		lv_snprintf (b + l, sizeof b - l, O ? "%s%s %s" : "%s%s %s free", l ? "   " : "", s_V.vol[i].name, f);
	}
	if (O) { upper (b); th_titlebar_set (s_TitleBar, "PROJECT", b, 0); }
	else set (s_Title, b, TH->win_dim);
	// the job's line
	if (!s_V.valid) set (s_Status, O ? "NO BUNDLE ENGINE" : "No bundle engine in this build", c_alert ());
	else if (s_bEjecting && s_V.eject_n != s_nEjectSeen) { s_bEjecting = 0; set (s_Status, s_V.eject_msg, c_alert ()); }
	else if (s_bEjecting) set (s_Status, O ? "EJECTING..." : "Ejecting...", c_ink ());
	else if (s_V.phase == SPJ_IDLE && s_V.eject_n != s_nEjectSeen && s_V.eject_msg[0]) set (s_Status, s_V.eject_msg, c_alert ());
	else if (s_V.phase == SPJ_IDLE && s_V.msg[0]) set (s_Status, s_V.msg, c_dim ());
	else if (s_V.phase == SPJ_IDLE && s_nListing == L_TRASH) set (s_Status, s_V.prj_last[0] ? s_V.prj_last : "Tap one, then RESTORE", c_dim ());
	else if (s_V.phase == SPJ_IDLE) set (s_Status, s_nListing ? "Tap a bundle, then IMPORT IT" : "Tap one, then a key below", c_dim ());
	else
	{
		lv_snprintf (b, sizeof b, "%s %s", s_V.job == SPJ_JOB_IMPORT ? "IMPORT" : s_V.job == SPJ_JOB_DELETE ? "DELETE" : "EXPORT", s_V.key);
		set (s_Status, b, c_ink ());
	}
}

// ---- the dialog
static const char *phase_name (int p)
{
	static const char *const n[] = {"IDLE", "FLUSH", "COLLECT", "HASH", "WRITE", "SERVE", "VERIFY", "PLAN", "READY", "BACKUP",
					"STAGE", "COMMIT", "LINK", "DELETE", "DONE", "FAILED", "CANCELLED"};
	return p >= 0 && p <= SPJ_CANCELLED ? n[p] : "?";
}
static int dlg_mode (void)
{
	int p = s_V.phase;
	if (s_nDelete >= 0 && p == SPJ_IDLE) return D_DELETE;
	if (p == SPJ_SERVE || running (p)) return D_PROGRESS;
	if (p == SPJ_READY && s_V.have_plan) return D_SUMMARY;
	if (p == SPJ_DONE) return D_DONE;
	if (p == SPJ_FAILED) return D_FAILED;
	if (p == SPJ_CANCELLED) return D_CANCELLED;
	return D_NONE;
}
static void dlg_line (int i, const char *t, uint32_t rgb) { if (i < NDLG) set (s_DlgText[i], t, rgb); }
static void dlg_update (void)
{
	const int O = oled ();
	int m = dlg_mode ();
	char b[320], x[24], y[24];
	s_nDlg = m;
	if (m == D_NONE)
	{
		lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_remove_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < NDLG; i++) dlg_line (i, "", c_ink ());
	lv_obj_add_flag (s_DlgBar, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < 4; i++) lv_obj_add_flag (s_DlgMach[i], LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_DlgNo, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag (s_DlgYes, LV_OBJ_FLAG_HIDDEN);
	th_key_enable (s_DlgYes, 1);
	th_key_enable (s_DlgNo, 1);
	th_key_kind (s_DlgYes, m == D_DELETE ? TH_KEY_DANGER | TH_KEY_FILL : TH_KEY_ACCENT);	// a delete's confirm filled (T16)
	switch (m)
	{
	case D_PROGRESS:
	{
		lv_snprintf (b, sizeof b, "%s %s", s_V.job == SPJ_JOB_IMPORT ? "Import" : s_V.job == SPJ_JOB_DELETE ? "Delete" : "Export", s_V.key[0] ? s_V.key : s_V.file);
		dlg_line (0, b, c_ink ());
		if (s_V.phase == SPJ_SERVE) { dlg_line (1, "A download from the web page runs", c_dim ()); }
		else
		{
			lv_snprintf (b, sizeof b, "%s", phase_name (s_V.phase));
			dlg_line (1, b, c_ink ());
			lv_obj_remove_flag (s_DlgBar, LV_OBJ_FLAG_HIDDEN);
			th_meter_set (s_DlgBar, s_V.total ? (int) (s_V.done * 1000 / s_V.total) : 0);
			if (s_V.total > 4096 && s_V.phase != SPJ_COMMIT && s_V.phase != SPJ_DELETE)
			{
				size_text (x, sizeof x, s_V.done), size_text (y, sizeof y, s_V.total);
				lv_snprintf (b, sizeof b, O ? "%s OF %s" : "%s of %s", x, y);
			}
			else if (s_V.total) lv_snprintf (b, sizeof b, "%u of %u", (unsigned) s_V.done, (unsigned) s_V.total);
			else b[0] = 0;
			dlg_line (3, b, c_dim ());
		}
		if (s_V.waiting[0])
		{
			lv_snprintf (b, sizeof b, "WAITING (%s)", s_V.waiting);
			dlg_line (4, b, c_alert ());
			dlg_line (5, !strcmp (s_V.waiting, "PLAYING") || !strcmp (s_V.waiting, "SONG REC") ? "It goes on by itself at STOP" :
				  !strcmp (s_V.waiting, "RECORDING") ? "It goes on when the recorder stops" : "The streams first: a moment", c_dim ());
		}
		lv_obj_add_flag (s_DlgYes, LV_OBJ_FLAG_HIDDEN);
		th_key_text (s_DlgNo, O ? "CANCEL" : "Cancel");
		th_key_enable (s_DlgNo, s_V.phase != SPJ_COMMIT && s_V.phase != SPJ_LINK && s_V.phase != SPJ_DELETE);
		break;
	}
	case D_SUMMARY:
	{
		if (s_V.plan_slot && s_V.plan_pick)
			lv_snprintf (b, sizeof b, O ? "IMPORT %s: NO EMPTY SLOT" : "Import %s: no empty slot", s_V.plan_from);
		else if (s_V.plan_slot)	// slot mode: the plan's slot (another one: the web page)
			lv_snprintf (b, sizeof b, O ? "IMPORT %s TO SLOT %03d" : "Import %s to slot %03d", s_V.plan_from, s_V.plan_slot);
		else
			lv_snprintf (b, sizeof b, O ? "IMPORT %s AS %s" : "Import %s as %s", s_V.plan_from, s_V.plan_name);
		dlg_line (0, b, c_ink ());
		size_text (x, sizeof x, s_V.plan_new), size_text (y, sizeof y, s_V.plan_reused);
		lv_snprintf (b, sizeof b, O ? "NEW %s (%u FILES), ON THE CARD %s" : "new %s (%u files), already on the card %s (%u files)", x, s_V.plan_write, y, s_V.plan_reuse);
		dlg_line (1, b, c_ink ());
		lv_snprintf (b, sizeof b, O ? "%u SAMPLES, %u SONGS, %u PATTERNS%s" : "%u samples, %u songs, %u patterns%s", s_V.plan_samples, s_V.plan_songs,
			     s_V.plan_patterns, s_V.plan_missing ? (O ? ", SOME MISSING" : ", some samples missing") : "");
		dlg_line (2, b, c_dim ());
		if (s_V.plan_pick && s_V.plan_slot) dlg_line (3, O ? "EVERY SLOT IS TAKEN: PICK ONE ON THE WEB PAGE" : "Every slot holds a project: pick one on the web page (Project tab)", c_alert ());
		else if (s_V.plan_clash && s_V.plan_slot) dlg_line (3, O ? "THAT SLOT HAS SIDECAR FILES: REPLACE ON THE WEB PAGE" : "That slot has Sidecar files: replace them on the web page (Project tab)", c_alert ());
		else if (s_V.plan_clash) { lv_snprintf (b, sizeof b, O ? "%s IS ON THE CARD: AS %s" : "a project %s is on the card: imported as %s", s_V.plan_from, s_V.plan_name); dlg_line (3, b, c_alert ()); }
		dlg_line (4, s_V.nmach ? (O ? "MACHINES (CODE) - TAP TO LEAVE OUT:" : "Machines (code from this bundle) - tap to leave one out:") : "", c_dim ());
		for (int i = 0; i < s_V.nmach && i < 4; i++)
		{
			const spj_mach_t *mc = &s_V.mach[i];
			int can = mc->action == SPJ_PKG_INSTALL || mc->action == SPJ_PKG_SIDE;
			static const char *const act[] = {"NEW", "ON CARD", "CONFLICT", "BESIDE", "OUT", "NOT PI", "REFUSED", "..."};
			lv_snprintf (b, sizeof b, "U%d %s", mc->slot, mc->name);	// the slot and name, its action on a second line
			th_key_text (s_DlgMach[i], b);
			th_key_sub (s_DlgMach[i], can && s_MachOff[i] ? "OUT" : act[mc->action & 7]);
			th_key_check (s_DlgMach[i], can && !s_MachOff[i]);
			th_key_enable (s_DlgMach[i], can);
			lv_obj_remove_flag (s_DlgMach[i], LV_OBJ_FLAG_HIDDEN);
		}
		lv_snprintf (b, sizeof b, O ? "RYTM: %s" : "The Rytm: %s", s_V.plan_step);
		dlg_line (6, b, c_ink ());
		th_key_text (s_DlgNo, O ? "CANCEL" : "Cancel");
		th_key_text (s_DlgYes, O ? "IMPORT" : "Import");
		th_key_enable (s_DlgYes, !(s_V.plan_slot && (s_V.plan_clash || s_V.plan_pick)));
		break;
	}
	case D_DONE:
		dlg_line (0, s_V.job == SPJ_JOB_IMPORT ? (O ? "IMPORTED" : "Imported") : O ? "DONE" : "Done", c_ink ());
		dlg_line (1, s_V.msg, c_dim ());
		if (s_V.job == SPJ_JOB_IMPORT && s_V.have_plan && s_V.plan_slot)
		{
			lv_snprintf (b, sizeof b, O ? "ITS SIDECAR FILES ARE IN SLOT %03d" : "Its Sidecar files are in slot %03d", s_V.plan_slot);
			dlg_line (3, b, c_alert ());
			dlg_line (4, "The Sidecar finds them by the Rytm's project slot.", c_dim ());
			dlg_line (6, s_V.plan_step, c_ink ());
		}
		else if (s_V.job == SPJ_JOB_IMPORT && s_V.have_plan)
		{
			lv_snprintf (b, sizeof b, O ? "SAVE YOUR RYTM PROJECT AS %s" : "Save your Rytm project as %s", s_V.plan_name);
			dlg_line (3, b, c_alert ());
			dlg_line (4, "The Sidecar finds its files by the project's name.", c_dim ());
			dlg_line (6, s_V.plan_step, c_ink ());
		}
		lv_obj_add_flag (s_DlgNo, LV_OBJ_FLAG_HIDDEN);
		th_key_text (s_DlgYes, "OK");
		break;
	case D_FAILED:
	case D_CANCELLED:
	{
		dlg_line (0, m == D_FAILED ? (O ? "IT DID NOT WORK" : "It did not work") : (O ? "CANCELLED" : "Cancelled"), m == D_FAILED ? c_alert () : c_ink ());
		// "what happened - what to do": the core's message; its part after " - " on a line of its own
		lv_snprintf (b, sizeof b, "%s", s_V.msg);
		char *dash = strstr (b, " - ");
		if (dash) { *dash = 0; dlg_line (2, dash + 3, c_ink ()); }
		dlg_line (1, b, c_ink ());
		if (m == D_FAILED && !dash) dlg_line (2, strstr (b, "USB") ? "Plug the stick in again, then start it again" : "Nothing was changed: try again", c_dim ());
		lv_obj_add_flag (s_DlgNo, LV_OBJ_FLAG_HIDDEN);
		th_key_text (s_DlgYes, "OK");
		break;
	}
	case D_DELETE:
	{
		if (s_nDelete >= s_V.nproj) { s_nDelete = -1; dlg_update (); return; }
		const spj_project_t *p = &s_V.proj[s_nDelete];
		lv_snprintf (b, sizeof b, O ? "DELETE %s?" : "Delete %s?", p->rytm);
		dlg_line (0, b, c_ink ());
		lv_snprintf (b, sizeof b, O ? "PROJECT FILES (%u) - SAMPLES ARE KEPT" : "Its project files (%u) go - its samples are kept", p->files);
		dlg_line (1, b, c_ink ());
		dlg_line (2, "No undo (export it first to keep a copy)", c_dim ());
		th_key_text (s_DlgNo, O ? "CANCEL" : "Cancel");
		th_key_text (s_DlgYes, O ? "DELETE" : "Delete");
		th_key_enable (s_DlgYes, lv_tick_elaps (s_nDelAt) >= 1000);	// grey for a second
		break;
	}
	}
}

static void update_all (void)
{
	top_update ();
	rows_update ();
	info_update ();
	keys_update ();
	dlg_update ();
}

// ---- input
static void row_cb (lv_event_t *e)
{
	if (lv_event_get_code (e) != LV_EVENT_CLICKED) return;
	int r = (int) (intptr_t) lv_event_get_user_data (e), i = s_nTop + r;
	if (i < count ()) *sel () = i;
	update_all ();
}
static void dest_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) ud;
	if (idx < 0 || idx > s_V.nvol) return;
	s_nDest = idx;
	lv_snprintf (s_DestDrive, sizeof s_DestDrive, "%s", idx ? s_V.vol[idx - 1].drive : "");
	if (s_nListing) { s_nBSel = 0, s_nTop = 0; ask_lists (); }
	update_all ();
}
static void eject_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx; (void) ud;
	if (!s_nDest) return;
	char b[48];
	lv_snprintf (b, sizeof b, "bundle=eject:%s", s_V.vol[s_nDest - 1].name);
	s_bEjecting = 1, s_nEjectSeen = s_V.eject_n;
	cmd (b);
	s_nDest = 0, s_DestDrive[0] = 0;
	update_all ();
}
static void key_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int k = (int) (intptr_t) ud, n = count ();
	char b[240];
	switch (k)
	{
	case K_EXPORT:
		if (s_nListing == L_TRASH && s_nTSel < s_V.ntrash)
		{
			lv_snprintf (b, sizeof b, "bundle=restore:%u", s_V.trash[s_nTSel].id);
			cmd (b);
		}
		else if (!s_nListing && s_nSel < s_V.nproj)
		{
			lv_snprintf (b, sizeof b, "bundle=export:%s:%s", dest_token (), s_V.proj[s_nSel].key);
			cmd (b);
		}
		else if (s_nListing && s_nBSel < count () && !s_V.bund[s_nBSel].incomplete)
		{
			memset (s_MachOff, 0, sizeof s_MachOff);
			lv_snprintf (b, sizeof b, "bundle=verify:%s", s_V.bund[s_nBSel].path);
			cmd (b);
		}
		break;
	case K_IMPORT:
		s_nListing = s_nListing ? L_PROJECTS : L_BUNDLES, s_nTop = 0;
		if (s_nListing) s_nBSel = 0, ask_lists ();
		break;
	case K_TRASH:
		s_nListing = L_TRASH, s_nTop = 0, s_nTSel = 0;
		cmd ("bundle=list:card");
		break;
	case K_DELETE:
		if (!s_nListing && s_nSel < s_V.nproj && !s_V.proj[s_nSel].current) s_nDelete = s_nSel, s_nDelAt = lv_tick_get ();
		break;
	case K_UP: s_nTop -= s_nRows; if (s_nTop < 0) s_nTop = 0; *sel () = s_nTop; break;
	case K_DOWN: if (s_nTop + s_nRows < n) s_nTop += s_nRows; *sel () = s_nTop; break;
	}
	update_all ();
}
static void dlg_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int yes = ud != 0;
	char b[64];
	switch (s_nDlg)
	{
	case D_PROGRESS: if (!yes) cmd ("bundle=cancel"); break;
	case D_SUMMARY:
		if (yes && s_V.plan_slot && (s_V.plan_clash || s_V.plan_pick)) break;	// (replaced from the web page only)
		if (yes)
		{
			unsigned skip = 0;
			for (int i = 0; i < s_V.nmach && i < 4; i++) if (s_MachOff[i] && s_V.mach[i].slot) skip |= 1u << (s_V.mach[i].slot - 1);
			lv_snprintf (b, sizeof b, "bundle=import:%u:1", skip);
			cmd (b);
		}
		else { cmd ("bundle=cancel"); cmd ("bundle=finish"); }
		break;
	case D_DONE: case D_FAILED: case D_CANCELLED:
		cmd ("bundle=finish");
		s_nListing = 0, s_nTop = 0;
		break;
	case D_DELETE:
		if (yes && s_nDelete >= 0 && s_nDelete < s_V.nproj && lv_tick_elaps (s_nDelAt) >= 1000)
		{
			lv_snprintf (b, sizeof b, "bundle=delete:%s", s_V.proj[s_nDelete].key);
			cmd (b);
		}
		s_nDelete = -1;
		break;
	}
	update_all ();
}
static void mach_cb (lv_obj_t *o, int idx, void *ud)
{
	(void) o; (void) idx;
	int i = (int) (intptr_t) ud;
	if (i < s_V.nmach && (s_V.mach[i].action == SPJ_PKG_INSTALL || s_V.mach[i].action == SPJ_PKG_SIDE)) s_MachOff[i] = !s_MachOff[i];
	dlg_update ();
}
static void delete_tick (lv_timer_t *t)	// the DELETE key comes on after its second
{
	(void) t;
	if (s_nDlg == D_DELETE && s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN)) th_key_enable (s_DlgYes, lv_tick_elaps (s_nDelAt) >= 1000);
}

// ---- the page
void sidecar_project_create (void *parent, int y0, sidecar_project_cmd_t c)
{
	const int O = oled (), Wd = W (), Hd = H (), dx = TH->dx;
	s_Cmd = c;
	s_nVolsSeen = -1, s_nGenSeen = ~0u, s_nSeqSeen = ~0u, s_nDelete = -1, s_nDlg = D_NONE;
	s_nListing = 0, s_nSel = s_nBSel = s_nTSel = s_nTop = 0;	// (a theme switch starts the page over)
	if (!s_bHave) memset (&s_V, 0, sizeof s_V);
	s_bHave = 0;
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, Wd, Hd);
	s_Page = pg;
	// the top: the title (OLED: its bar carries the free space), the picker, the job's line, EJECT
	const int top = O ? 15 : 6, th = O ? 22 : 56, ky = Hd - (O ? 23 : 62), kh = O ? 22 : 56;
	const int ly = top + th + (O ? 3 : 8), lh = ky - ly - (O ? 2 : 8);
	if (O)
	{
		s_TitleBar = th_titlebar (pg, 0, 0, Wd, 13);
		th_titlebar_set (s_TitleBar, "PROJECT", "", 0);
		s_Title = 0;
		static const char *const m0[] = {"CARD", ""};
		s_Dest = th_seg (pg, 0, top, 140 + dx / 4, th, m0, TH_KEY | TH_KEY_SMALL, dest_cb, 0);
		s_Status = label_w (pg, 146 + dx / 4, top + 6, Wd - 146 - dx / 4 - 52, TH->f_micro, TH->dim, 0);
		s_Eject = th_key (pg, Wd - 48, top, 48, th, "EJECT", TH_KEY | TH_KEY_SMALL, eject_cb, 0);
	}
	else
	{
		s_TitleBar = 0;
		th_panel (pg, 8, top, Wd - 16, th, "", TH_PANEL_WINDOW);
		static const char *const m0[] = {"SD card", ""};
		s_Dest = th_seg (pg, 16, top + 4, 300 + dx / 4, th - 8, m0, TH_KEY, dest_cb, 0);
		th_seg_font (s_Dest, &lv_font_montserrat_16);
		s_Title = label_w (pg, 330 + dx / 4, top + 6, Wd - 330 - dx / 4 - 130, &lv_font_montserrat_14, TH->win_dim, 0);
		s_Status = label_w (pg, 330 + dx / 4, top + 28, Wd - 330 - dx / 4 - 130, &lv_font_montserrat_16, TH->win_ink, 0);
		s_Eject = th_key (pg, Wd - 120, top + 4, 104, th - 8, "Eject", TH_KEY, eject_cb, 0);
		th_key_font (s_Eject, &lv_font_montserrat_16);
	}
	lv_obj_add_flag (s_Eject, LV_OBJ_FLAG_HIDDEN);
	// the list (left) and the selection's details (right)
	const int lw = O ? Wd * 3 / 5 : (Wd - 24) * 3 / 5, lx = O ? 0 : 8, rx = lx + lw + (O ? 3 : 8), rw = Wd - rx - (O ? 0 : 8);
	const int pitch = O ? 21 : 44, hdr = O ? 0 : 8;
	s_nRows = (lh - hdr) / pitch;
	if (s_nRows > NROWS_MAX) s_nRows = NROWS_MAX;
	s_ListPanel = th_panel (pg, lx, ly, lw, lh, "", O ? TH_PANEL_GROUP : TH_PANEL_WINDOW);
	const int sw = O ? 52 : 100, ww = O ? 56 : 120;
	for (int r = 0; r < NROWS_MAX; r++)
	{
		s_Row[r] = 0;
		if (r >= s_nRows) continue;
		int y = ly + hdr + r * pitch;
		lv_obj_t *row = th_box (pg, lx + 1, y, lw - 2, pitch);
		lv_obj_set_style_bg_color (row, lv_color_hex (O ? TH->sel_bg : 0x2A3A4C), 0);
		lv_obj_set_style_bg_opa (row, LV_OPA_TRANSP, 0);
		lv_obj_add_flag (row, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb (row, row_cb, LV_EVENT_CLICKED, (void *) (intptr_t) r);
		s_Row[r] = row;
		const lv_font_t *fv = f_val ();
		int ty = (pitch - lv_font_get_line_height (fv)) / 2;
		s_RowName[r] = label_w (row, O ? 4 : 12, ty, lw - sw - ww - (O ? 10 : 30), fv, c_ink (), 0);
		s_RowSize[r] = label_w (row, lw - sw - ww - (O ? 4 : 12), ty, sw, f_cap (), c_dim (), 1);
		s_RowWhen[r] = label_w (row, lw - ww - (O ? 2 : 8), ty, ww - (O ? 2 : 4), f_cap (), c_dim (), 1);
		lv_obj_add_flag (row, LV_OBJ_FLAG_HIDDEN);
	}
	s_Empty = label_w (pg, lx, ly + lh / 2 - 8, lw, f_cap (), c_dim (), -1);
	th_panel (pg, rx, ly, rw, lh, O ? "" : "", O ? TH_PANEL_GROUP : TH_PANEL_WINDOW);
	const int ip = O ? 14 + TH->dy / 12 : 30 + TH->dy / 10;
	for (int k = 0; k < 6; k++) s_Info[k] = label_w (pg, rx + (O ? 4 : 14), ly + (O ? 4 : 12) + k * ip, rw - (O ? 8 : 28), k ? f_cap () : f_val (), c_ink (), 0);
	// the keys
	static const char *const kO[6] = {"EXPORT CARD", "IMPORT...", "DELETE", "UP", "DOWN", "TRASH"};
	static const char *const kC[6] = {"Export > card", "Import...", "Delete", "Up", "Down", "Trash"};
	const int g = O ? 2 : 8, kx0 = O ? 0 : 8, kw = Wd - 2 * kx0, uw = O ? 32 + dx / 8 : 80 + dx / 8;
	const int bw = (kw - 2 * uw - 5 * g) / 4;	// (four wide keys: EXPORT IMPORT DELETE TRASH, then UP DOWN)
	for (int k = 0; k < 6; k++)
	{
		int slot = k == K_TRASH ? 3 : k < 3 ? k : k + 1;
		int x = slot < 4 ? kx0 + slot * (bw + g) : kx0 + 4 * (bw + g) + (slot - 4) * (uw + g);
		int w = slot < 4 ? bw : uw;
		s_Key[k] = th_key (pg, x, ky, w, kh, O ? kO[k] : kC[k], k == K_DELETE ? TH_KEY_DANGER : k == K_EXPORT ? TH_KEY_ACCENT : TH_KEY,
				   key_cb, (void *) (intptr_t) k);
		if (!O) th_key_font (s_Key[k], &lv_font_montserrat_16);
	}
	// the dialog: over a layer that takes every other tap
	s_Scrim = th_box (pg, 0, 0, Wd, Hd);
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_CLICKABLE);
	if (!O)
	{
		lv_obj_set_style_bg_color (s_Scrim, lv_color_hex (0x000000), 0);
		lv_obj_set_style_bg_opa (s_Scrim, LV_OPA_50, 0);
	}
	lv_obj_add_flag (s_Scrim, LV_OBJ_FLAG_HIDDEN);
	const int dw = O ? 330 + dx / 2 : 680 + dx / 2, dh = O ? 196 + TH->dy / 2 : 384 + TH->dy / 2;
	s_Dlg = th_box (pg, (Wd - dw) / 2, (Hd - dh) / 2, dw, dh);
	if (O)
	{
		lv_obj_set_style_bg_color (s_Dlg, lv_color_hex (TH->bg), 0);
		lv_obj_set_style_bg_opa (s_Dlg, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Dlg, lv_color_hex (TH->ink), 0);
		lv_obj_set_style_border_width (s_Dlg, 1, 0);
	}
	else th_panel (s_Dlg, 0, 0, dw, dh, "", TH_PANEL_WINDOW);
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_CLICKABLE);
	const int lp = O ? 13 + TH->dy / 16 : 30 + TH->dy / 12, tx = O ? 6 : 20, tw = dw - 2 * tx;
	for (int i = 0; i < NDLG; i++)
	{
		int ly2 = (O ? 5 : 14) + (i < 6 ? i * lp : 5 * lp + (O ? 21 : 44) + (O ? 4 : 10));	// (6: under the machines' keys)
		s_DlgText[i] = label_w (s_Dlg, tx, ly2, tw, i ? f_cap () : f_val (), c_ink (), 0);
		if (i == 6 || i == 1)	// two lines: the summary's Rytm step, a done / failed message
		{
			lv_label_set_long_mode (s_DlgText[i], LV_LABEL_LONG_MODE_WRAP);
			lv_obj_set_height (s_DlgText[i], 2 * lv_font_get_line_height (f_cap ()));
		}
	}
	s_DlgBar = th_meter (s_Dlg, tx, (O ? 5 : 14) + 2 * lp + 1, tw, O ? 8 : 16, 1000, TH_METER_H, O ? TH->ink : TH->accent);
	const int mk = O ? 21 : 44, mw = (tw - 3 * (O ? 2 : 8)) / 4;
	for (int i = 0; i < 4; i++)
	{
		s_DlgMach[i] = th_key (s_Dlg, tx + i * (mw + (O ? 2 : 8)), (O ? 5 : 14) + 5 * lp - (O ? 2 : 4), mw, mk, "", TH_KEY | TH_KEY_HOLD | TH_KEY_SMALL,
				       mach_cb, (void *) (intptr_t) i);
		if (!O) th_key_font (s_DlgMach[i], &lv_font_montserrat_14);
	}
	const int bh = O ? 24 : 56, by = dh - bh - (O ? 5 : 16), bw2 = (tw - (O ? 8 : 24)) / 2;
	s_DlgNo = th_key (s_Dlg, tx, by, bw2, bh, "CANCEL", TH_KEY, dlg_cb, (void *) 0);
	s_DlgYes = th_key (s_Dlg, tx + bw2 + (O ? 8 : 24), by, bw2, bh, "OK", TH_KEY_ACCENT, dlg_cb, (void *) 1);
	if (!O) { th_key_font (s_DlgNo, &lv_font_montserrat_20); th_key_font (s_DlgYes, &lv_font_montserrat_20); }
	lv_obj_add_flag (s_Dlg, LV_OBJ_FLAG_HIDDEN);
	static lv_timer_t *s_Tick;
	if (!s_Tick) s_Tick = lv_timer_create (delete_tick, 250, 0);
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
	update_all ();
}

void sidecar_project_show (int on)
{
	if (!s_Page) return;
	if (on && lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN)) ask_lists ();
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else { lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN); s_nDelete = -1; }
}

int sidecar_project_shown (void) { return s_Page && !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN); }

void sidecar_project_set (const sidecar_project_view_t *v)
{
	if (!s_Page) return;
	int lists = !s_bHave || v->gen != s_nGenSeen || v->reading != s_V.reading || v->eject_n != s_V.eject_n || strcmp (v->src, s_V.src);
	int job = !s_bHave || v->seq != s_nSeqSeen || v->nvol != s_V.nvol;
	if (v->phase != SPJ_READY) memset (s_MachOff, 0, sizeof s_MachOff);
	if (!lists && !job && s_nDlg != D_DELETE) return;
	s_V = *v;
	s_bHave = 1;
	s_nGenSeen = v->gen, s_nSeqSeen = v->seq;
	update_all ();
}

int sidecar_project_listing (void) { return s_nListing; }
int sidecar_project_dialog (void) { return s_nDlg; }
int sidecar_project_selected (void) { return *sel (); }

static int centre (lv_obj_t *o, int *x, int *y)
{
	if (!o || lv_obj_has_flag (o, LV_OBJ_FLAG_HIDDEN)) return 0;
	for (lv_obj_t *p = lv_obj_get_parent (o); p; p = lv_obj_get_parent (p)) if (lv_obj_has_flag (p, LV_OBJ_FLAG_HIDDEN)) return 0;
	lv_area_t a;
	lv_obj_get_coords (o, &a);
	*x = (a.x1 + a.x2) / 2, *y = (a.y1 + a.y2) / 2;
	return 1;
}
int sidecar_project_hit (int what, int i, int *x, int *y)
{
	switch (what)
	{
	case SPJ_HIT_ROW: return i >= 0 && i < s_nRows && centre (s_Row[i], x, y);
	case SPJ_HIT_KEY: return i >= 0 && i < 6 && centre (s_Key[i], x, y);
	case SPJ_HIT_DEST: return i >= 0 && i < th_seg_count (s_Dest) && centre (th_seg_item (s_Dest, i), x, y);
	case SPJ_HIT_EJECT: return centre (s_Eject, x, y);
	case SPJ_HIT_DLG: return centre (i ? s_DlgYes : s_DlgNo, x, y);
	case SPJ_HIT_MACH: return i >= 0 && i < 4 && centre (s_DlgMach[i], x, y);
	}
	return 0;
}
