/* core/ui/page_keys.h - the Sidecar's shortcuts on the device, as one table a device (device-agnostic host side, the
 * protocol rule: a future device - an Analog Four MKII - adds its own table and its firmware mod; nothing else hard-codes
 * a key). Read by the Rytm screen's KEYS page, UP's combo map, the SIDECAR MENU's shortcut column and its footers
 * (core/ui/page_menu.cpp, remote.cpp), the page requests (remote.cpp display_remote_page, pi/engine/voice_pi.cpp) and the
 * touchscreen's navigation captions and its map (pi/ui/sidecar_ui.c). Plain C, so the Pi's UI reads it too.
 *
 * The only device so far: the Analog Rytm MKII with rytm-mods 0026 (rui1046; UP + AMP rui1047; POLY CONFIG rui1048). Its one modifier is UP (TRK is stock's
 * busiest key; UP held alone shows the map): UP + T1..T13 request a page (49 1 <n>), UP + a key is a combo the Rytm
 * handles itself (it tells the host by 30 / 2D as before). The TRK / FUNC forms of rui1026..1045 still work (legacy).
 * rui1050: the SEQ page's own gestures (TRIG cycling its pages, a lock page's long push / FUNC + push) are KEYS rows
 * only (no UP combo: UP's map stays one page).
 * rui1048: POLY CONFIG is an item of the Rytm's own track menu (FUNC + CHRO, the last item), not a combo: the row "poly"
 * names where it is (the KEYS page, the SIDECAR MENU's shortcut column); the Rytm asks for the page by 30 page 0x20 + t.
 * rui1051: CHOP's editor opens from its SRC view (a push of encoder B..H: no new UP combo); its own keys are KEYS rows
 * only.
 * rui1052 (the user's report on rui1051): T1..T8 are the touchscreen's sidebar top to bottom, and UP's map is read from
 * the rows, never from a key number - see the table below and core/ui/page_menu.cpp map_draw.
 * U1 NOTE EDIT (0.3.0): DATA (the LEVEL / DATA encoder) on a poly track's SEQ page, its push, FUNC + NO: KEYS rows only
 * (no mod change: 0026 forwards encoder 8 and key 0x78 when the page's mask names them).
 * ux1027 (0.3.0, the user): DOWN is the poly SEQ page's menu (a KEYS row); YES there is the Rytm's (its YES + KIT /
 * PLAY / SCNE quick saves), caught only under a question, with FUNC held first (FUNC + YES: UNDO) or a trig held.
 * SEQ, INSERT (UP + FX / AMP), LFO (UP + LFO) and POLY replace the Rytm's screen: no LEFT / RIGHT siblings.
 * updown1029 (the user, 2026-10-29): the menu is FUNC + DOWN; UP / DOWN alone are the Rytm's on the SEQ page (CHRO: the
 * pads' octave), caught only under a question or with FUNC held.
 */
#ifndef CORE_UI_PAGE_KEYS_H
#define CORE_UI_PAGE_KEYS_H

typedef struct {
	unsigned char code;	/* the device's page request (49 1 <code>); on the Rytm UP + T<code> */
	const char *name;	/* the page: the touchscreen's page of that name, its navigation caption, the TOUCH: popup */
	const char *rytm;	/* its title on the device's own screen (remote_show_page; "SIDECAR": kStatusTitle), 0: the
				   touchscreen's alone. A touchscreen that has the page takes it (remote_display_owns) */
	const char *more;	/* the KEYS page's row after the name ("" none) */
} sc_page_key_t;

typedef struct {
	const char *id;		/* what the host asks the table for (sc_combo_keys), 0: a row of the KEYS page only */
	const char *keys;	/* as the KEYS page writes it: "UP+TRIG" (T+: a trig held first, on a poly track) */
	const char *what;	/* the KEYS page's text */
	const char *map;	/* UP's combo map: the key and its page in at most 12 characters ("TRIG SEQ"), 0: not on it */
	const char *legacy;	/* the older form that still works ("TRK+TRIG"), 0 none */
	const char *rytm;	/* the page it opens on the device's screen (the SIDECAR MENU's shortcut column), 0 none */
	const char *alone;	/* rui1052: as `map`, on the map drawn with no touchscreen connected - the rows the
				   touchscreen's own pages free there (page_menu.cpp map_draw), 0: not on it either */
} sc_combo_t;

typedef struct {
	const char *device;
	const char *modifier;	/* held alone: the combo map */
	const char *page_keys;	/* a page request's keys before its code: "UP+T" */
	const char *map_key;	/* the map's prefix of a page request: "T" */
	int npages;
	const sc_page_key_t *pages;
	int ncombos;
	const sc_combo_t *combos;
	int nonly;		/* rui1051: the Rytm-screen pages (ExtraPage titles) only their keys open: out of the SIDECAR
				   MENU's list and off the LEFT / RIGHT ring */
	const char *const *only;
} sc_keymap_t;

/* the Analog Rytm MKII (rytm-mods 0026 rui1046). Stock 1.73 leaves every one of these free [measured, rytm-emu,
   rytm-mods tools/emu/rled_emu.py stockup / stockupkeys]: UP held + the key leaves what the key alone leaves (in a list
   what UP tapped and then the key leave).
   rui1052: T1..T8 are the touchscreen's sidebar, top to bottom (pi/ui/sidecar_ui.c kNavPage: HOME SEQ PAT VOICE FX SONG
   REC SET - the sidebar's FX is INSERT FX here); T9..T13 are the pages with no sidebar row, on the keys they have had
   since rui1046 (SCOPE STATUS SAMPLE PROJECT POOL: only SAMPLE moved, into the row SONG left). Which screen a page
   belongs to is the row's `rytm` field, not its number (0: the touchscreen's alone - before rui1052 that was the
   leading run 1..6, and UP's map read the number). */
static const sc_page_key_t sc_rytm_mk2_pages[] = {
	{1, "HOME", 0, ""},		{2, "SEQ", 0, ""},		{3, "PAT", 0, ""},
	{4, "VOICE", 0, ""},		{5, "INSERT FX", 0, ""},	{6, "SONG", "SONG", ""},
	{7, "REC", "REC", ""},		{8, "SET", 0, ""},		{9, "SCOPE", "SCOPE", ""},
	{10, "STATUS", "SIDECAR", ""},	{11, "SAMPLE", "SAMPLE", ", AGAIN: AUTO"},
	{12, "PROJECT", "PROJECT", ""},	{13, "POOL", "POOL", ""},
};
static const sc_combo_t sc_rytm_mk2_combos[] = {
	{"pages", "UP+YES", "SIDECAR (YES: MENU)", "YES PAGES", "TRK+YES", 0},
	{"seq", "UP+TRIG", "SEQ (POLY TRACK)", "TRIG SEQ", "TRK+TRIG", "SEQ"},
	{"srclist", "UP+SRC", "SOURCE LIST", "SRC LIST", 0, 0},
	{"lfo", "UP+LFO", "SIDECAR LFOS", "LFO LFOS", "TRK+LFO", "LFO"},
	{"ins", "UP+FX", "INSERT FX", "FX INSERT FX", "TRK+FX", "INSERT"},
	{"out", "UP+AMP", "INSERT OUT VIEW", "AMP OUT", 0, 0},
	{"copy", "T+UP+REC", "COPY STEPS (SEQ)", 0, "T+FUNC+REC", 0, "T+REC COPY"},
	{"paste", "T+UP+STOP", "PASTE STEPS", 0, "T+FUNC+STOP", 0, "T+STOP PASTE"},
	{"clear", "T+UP+PLAY", "CLEAR STEPS", 0, "T+FUNC+PLAY", 0, "T+PLAY CLEAR"},
	{"poly", "FUNC+CHRO", "MENU: POLY CONFIG", 0, 0, "POLY"},
	{"chop", "PUSH ON SRC", "CHOP EDITOR (B..H)", 0, 0, "CHOP"},	/* CHOP: a push of encoder B..H on its SRC view */
	{0, "FUNC+PAGE", "SCALE (SEQ)", 0, 0, 0},
	{0, "AMP AMP", "SENDS (AMP 2/2)", 0, 0, 0},
	{0, "FX: SRC/SMPL", "SEND FX 1 / 2", 0, 0, 0},
	{0, "TRIG+SRC..FX", "STEP LOCKS (SEQ)", 0, 0, 0},
	{0, "TRIG+YES", "STEP PREVIEW (SEQ)", 0, 0, 0},
	{0, "TRIG", "SEQ: TRACK PAGES", 0, 0, 0},	/* rui1050: DEFAULTS SCALE RANDOM */
	{0, "T+TRIG", "SEQ: STEP PAGES", 0, 0, 0},	/* rui1050: TRIG RTRG FX */
	{0, "LONG PUSH", "LOCK COND (SEQ)", 0, 0, 0},	/* rui1050: on a lock page */
	{0, "FUNC+PUSH", "LOCK SLIDE (SEQ)", 0, 0, 0},
	{0, "DATA", "NOTE EDIT (SEQ)", 0, 0, 0},	/* U1: DATA walks every note of a poly track (core/seq note_edit) */
	{0, "DATA PUSH", "SELECT NOTE (SEQ)", 0, 0, 0},	/* FUNC + push: all / none */
	{0, "FUNC+NO", "DELETE NOTES (SEQ)", 0, 0, 0},	/* in NOTE EDIT (2+ notes: again to confirm) */
	{0, "FUNC+DOWN", "MENU (POLY SEQ)", 0, 0, 0},	/* ux1027: the poly track's menu (YES before: YES is the Rytm's there now);
							   updown1029: FUNC + DOWN (DOWN alone: CHRO's octave) */
	{0, "FUNC+YES", "UNDO (POLY SEQ)", 0, 0, 0},	/* review round 1: the touchscreen's history; fix1028: the whole poly SEQ page;
							   ux1027: FUNC first (YES caught only while FUNC is held) */
	{0, "FUNC+YES", "FILE INTO A ROW", 0, 0, 0},
	{0, "STOP STOP", "SIDECAR FX CLEARED", 0, 0, 0},
	{0, "NO", "BACK A LEVEL", 0, 0, 0},
	{0, "CHOP: PAGE", "EDIT SLICE 1 SLICE 2", 0, 0, 0},	/* the CHOP editor's own keys (core/ui/page_chop.cpp) */
	{0, "CHOP: PUSH A-D", "ADD DEL MERGE UNDO", 0, 0, 0},
	{0, "CHOP: PUSH E-H", "GRID TRANS LAZY RSMP", 0, 0, 0},
	{0, "CHOP: T1..T16", "SLICE (LAZY: TAP)", 0, 0, 0},
};
/* rui1051 (the user's report on rui1050): pages that work on what the selected track holds - its poly sequence (SEQ,
   UP+TRIG), its insert (INSERT, UP+FX / UP+AMP), the pool its machine plays from (POOL, UP+T13 or SRC LIST's), its slices
   (CHOP, a push on its SRC view) - open from that track by their keys, not from the list: browsing to them on another
   track's context showed the wrong track's page. POLY CONFIG stays in the list (its key is the last item of stock's
   track menu: the list is where it is found). */
static const char *const sc_rytm_mk2_only[] = {"SEQ", "INSERT", "POOL", "CHOP"};
static const sc_keymap_t sc_rytm_mk2 = {
	"Analog Rytm MKII", "UP", "UP+T", "T",
	(int) (sizeof sc_rytm_mk2_pages / sizeof sc_rytm_mk2_pages[0]), sc_rytm_mk2_pages,
	(int) (sizeof sc_rytm_mk2_combos / sizeof sc_rytm_mk2_combos[0]), sc_rytm_mk2_combos,
	(int) (sizeof sc_rytm_mk2_only / sizeof sc_rytm_mk2_only[0]), sc_rytm_mk2_only,
};

/* the device connected (the only one so far) */
static inline const sc_keymap_t *sc_keymap (void) { return &sc_rytm_mk2; }

static inline int sc_streq (const char *a, const char *b)
{
	if (!a || !b) return 0;
	while (*a && *a == *b) a++, b++;
	return *a == *b;
}
/* the page request of that code, 0 none */
static inline const sc_page_key_t *sc_page_of_code (int code)
{
	const sc_keymap_t *m = sc_keymap ();
	for (int i = 0; i < m->npages; i++) if (m->pages[i].code == code) return &m->pages[i];
	return 0;
}
/* the page request of the page called name (sc_page_key_t.name), 0 none */
static inline const sc_page_key_t *sc_page_of_name (const char *name)
{
	const sc_keymap_t *m = sc_keymap ();
	for (int i = 0; i < m->npages; i++) if (sc_streq (m->pages[i].name, name)) return &m->pages[i];
	return 0;
}
/* the highest page request code */
static inline int sc_page_max (void)
{
	const sc_keymap_t *m = sc_keymap ();
	int n = 0;
	for (int i = 0; i < m->npages; i++) if (m->pages[i].code > n) n = m->pages[i].code;
	return n;
}
/* 1: the page of that title (an ExtraPage's) opens only by its keys (rui1051: not in the SIDECAR MENU, off the ring) */
static inline int sc_shortcut_only (const char *title)
{
	const sc_keymap_t *m = sc_keymap ();
	for (int i = 0; i < m->nonly; i++) if (sc_streq (m->only[i], title)) return 1;
	return 0;
}
/* a combo's keys by its id ("pages" -> "UP+YES"), "" none */
static inline const char *sc_combo_keys (const char *id)
{
	const sc_keymap_t *m = sc_keymap ();
	for (int i = 0; i < m->ncombos; i++) if (sc_streq (m->combos[i].id, id)) return m->combos[i].keys;
	return "";
}

#endif
