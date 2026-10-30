// pi/ui/sidecar_wgview.h - the Pi display's machine view for WAVETABLE, GRANULAR and MULTISAMPLE (docs/GRANULAR.md "Displays"):
// GRANULAR - the source's outline (a card sample, a built-in, or the live ring with the newest audio right), the head
// (POS + ANIM + the scan) and every sounding grain as a short bar at its place, as bright and as tall as its level;
// WAVETABLE - the table as a stack of frames (pseudo-3D, front = the first) with the frame at POS (morphed) on top
// and the place in the table; MULTISAMPLE (touch2) - the instrument's key zones over a keyboard of its span, the keys
// sounding lit. Each machine's POOL (core/lib/pools.h; the design: docs/superpowers/specs/2026-10-22-pools-tonverk-design.md):
// GRANULAR's SAMPLES, WAVETABLE's WAVETABLES, MULTISAMPLE's INSTRUMENTS - 8 banks x 128 rows, row 0 OFF; and the POOL
// page, the three lists for housekeeping. LVGL only (builds on the Mac too, pi/ui/host); both themes, both sizes. The
// caller hands it a view (sidecar_wgview_set, ~10 times a second while it is shown); it redraws only when the view's gen
// changes. Draw cost: filled rects and text only (no anti-aliasing: the OLED theme's dots).
#ifndef SIDECAR_WGVIEW_H
#define SIDECAR_WGVIEW_H

#ifdef __cplusplus
extern "C" {
#endif

#define WGV_COLS	200		// the outline's / a frame's columns at most
#define WGV_GRAINS	128		// core/granular kMaxGrains
#define WGV_STACK	9		// WAVETABLE frames drawn behind the current one
#define WGV_ROWS	128		// a bank's rows (row 0 OFF)
#define WGV_CODES	1024		// a list's codes (bank * 128 + row)
#define WGV_NAME	28		// a row's name shown
#define WGV_LIB		512		// the browser's rows at most (a folder's first 512)
#define WGV_LNAME	28

#define WGV_ZONES	32		// MULTISAMPLE's key zones drawn at most

enum { WGV_NONE = 0, WGV_GRANULAR = 1, WGV_WAVETABLE = 2, WGV_MULTISAMPLE = 3 };
// the three POOL lists (= core::pools::List, the Rytm's 47 list numbers, srclib's browser kinds)
enum { WGV_SAMPLES = 0, WGV_TABLES = 1, WGV_INSTR = 2, WGV_LISTS = 3 };
// a row's entry (= core::pools::Type)
enum { WGV_RT_EMPTY = 0, WGV_RT_OFF, WGV_RT_SAMPLE, WGV_RT_INPUT, WGV_RT_BUILTIN, WGV_RT_INSTR };
// a row's state (flags): its audio in RAM, being loaded (P4), its file missing (silent: "!name"), changed on the card
// ("~name", plays; one tap accepts), it failed to load, it was seen playing in this project (the used set); its file
// stereo / mono once a machine has read it (docs/STEREO.md: the ST / M tag)
enum { WGV_RF_RAM = 1, WGV_RF_LOADING = 2, WGV_RF_MISSING = 4, WGV_RF_CHANGED = 8, WGV_RF_FAILED = 16, WGV_RF_USED = 32,
       WGV_RF_ST = 64, WGV_RF_MONO = 128 };
enum { WGV_LIB_DIR = 1, WGV_LIB_VIRT = 2 };	// a browser item: a folder; a virtual one (@INPUTS/ @BUILT-IN/ and theirs)

typedef struct
{
	int		kind;			// WGV_*
	int		voice;			// 0..7
	int		track;			// the Rytm track (0..11) it follows, -1 none known
	char		name[32];		// the source / table name
	int		ncols;			// columns filled below
	// GRANULAR
	int		live, frozen;
	float		head;			// 0..1
	float		anim;			// ANIM's depth 0..1 (0 off)
	char		anim_text[8], hold_text[8];	// "1BAR", "FRZ"
	float		wmn[WGV_COLS], wmx[WGV_COLS];	// the outline, -1..1
	int		n;
	float		gpos[WGV_GRAINS], gamp[WGV_GRAINS];	// 0..1
	unsigned char	grev[WGV_GRAINS];
	// WAVETABLE
	int		frames;			// the table's frames
	float		pos;			// 0..1
	float		cur[WGV_COLS];		// the frame at POS
	float		stack[WGV_STACK][WGV_COLS];	// frames 0, 1/8 .. 1 of the table
	// MULTISAMPLE (touch2): the instrument's zones (each key range once, its velocity layers counted), the keys sounding
	int		ms_ready;		// 1: the zones are known (the instrument is loaded)
	int		ms_nz;
	unsigned char	ms_lo[WGV_ZONES], ms_hi[WGV_ZONES], ms_root[WGV_ZONES], ms_vel[WGV_ZONES];
	unsigned char	ms_keys[16];		// bit n: key n sounding
	// ---- the POOL: one bank of one list (the one sidecar_wgview_list_id () and sidecar_wgview_bank () ask for)
	int		list;			// WGV_SAMPLES .. (-1: none filled)
	int		bank;			// the bank the rows are of (0 A .. 7 H)
	char		rname[WGV_ROWS][WGV_NAME];	// a row's name ("" empty; row 0 "OFF")
	unsigned char	rtype[WGV_ROWS];	// WGV_RT_*
	unsigned char	rflags[WGV_ROWS];	// WGV_RF_*
	unsigned char	rusers[WGV_ROWS];	// voices on it now (P3: "USED BY 3 SOUNDS")
	int		count[WGV_LISTS];	// each list's entries (the POOL page's tabs)
	int		locked;			// the list's file could not be read: no edits (lock_why)
	char		lock_why[32];
	int		playing;		// the Rytm plays (P3: a row in use is not replaced / unloaded then)
	int		sel;			// the code the voice plays (-1 none / not this list)
	char		note[24];		// the playing row's note: "LOADING", "TOO LONG: FIRST 87 S", its length
	// the pool RAM ("MB and %", core::pools::ram (); ram_known 0 until the shared cache counts: shown "-")
	int		ram_known;
	unsigned	ram_mb, ram_budget_mb;	// used (the samples' cache + the tables), the budget
	int		ram_pct;
	// SELECT UNUSED (while sidecar_wgview_selecting ()): the list's rows not seen playing (core::pools::select_unused)
	int		nunused;
	short		unused[WGV_CODES];
	char		warn[64];		// core::pools::kUnusedWarning
	// the browser (core/lib/srclib.h) of the list's machine: its folder, folders first, then the files; the root shows
	// the virtual folders the list takes (@INPUTS/ @BUILT-IN/)
	int		lib_n;			// rows filled (at most WGV_LIB)
	int		lib_total;		// entries in the folder
	int		lib_done;		// the folder is listed whole
	char		lib_cwd[96];		// "" the card's root
	char		lib_name[WGV_LIB][WGV_LNAME];
	unsigned char	lib_flags[WGV_LIB];	// WGV_LIB_*
	short		lib_row[WGV_LIB];	// one entry per hash: the code already holding the file (-1 none)
	unsigned	lib_gen;		// a new folder / listing / list
	unsigned	gen;			// changes with anything above (the view redraws only then)
} sidecar_wg_view_t;

void sidecar_wgview_create (void *parent, int y0);	// hidden until shown
void sidecar_wgview_show (int on);
int sidecar_wgview_shown (void);
void sidecar_wgview_set (const sidecar_wg_view_t *v);	// copied
void sidecar_wgview_want (int voice);			// the voice a tap picked (the provider's preference), -1 follow
int sidecar_wgview_wanted (void);
// The POOL (the machine view's POOL key, or the POOL page): a tab row A..H (all alike; a change sends
// "pool=smp|wt|inst:shown:B": takes go to the bank last shown), then the bank's rows "000 OFF", "001 name", empty rows
// "---"; states LOADING / RAM / MISSING ("!name") / CHANGED ("~name"), a marker for a row in use (voices on it now:
// filled; seen playing in this project: an outline). The title: the list and the RAM ("RAM 412 MB 41%", "-" unknown).
//   a tap: a row with content - the voice plays it ("gr=pick:V:C005" / "wt=pick:V:C005" / "msamp=pick:V:C005": the
//          Rytm's P1, 48); an empty row - FILES aimed at it: the list machine's browser ("browse=gr|wt|ms:open:I", "up",
//          "root"), a file / virtual item into the row ("pool=smp|wt|inst:assign:C005:I", I the browser's item; the next
//          empty row then), FILL B the folder's files into the bank's empty rows ("pool=smp|wt:fill:B:/folder"). A file
//          already in the list (one entry per hash) shows its row ("IN C004") and a tap goes there instead.
//   a long press on a filled row (the POOL page: a tap): REPLACE (FILES aimed at it) / UNLOAD ("pool=K:unload:C005") /
//          ACCEPT a changed file ("pool=K:accept:C005"). P3: a row in use asks first ("ROW C012 USED BY 3 SOUNDS.
//          REPLACE?" / "ROW C012 SEEN PLAYING IN THIS PROJECT. UNLOAD?"), YES sends with ":yes" appended; while the Rytm
//          plays a row in use is refused. After a replace / unload the message row offers UNDO ("pool=K:undo").
//   UNUSED (SELECT UNUSED): the rows not seen playing selected (a tap deselects one), with the warning; UNLOAD N sends
//          them ("pool=K:unload:A010:yes,A011:yes,..", a few commands of 19 rows) after a confirm.
// Commands through fn (sidecar_ui's).
void sidecar_wgview_set_command (void (*fn) (const char *cmd));
void sidecar_wgview_set_page (void (*fn) (int open));	// ALL POOLS opens the POOL page (1), its DONE leaves it (0)
void sidecar_wgview_list (int on);			// the machine view's POOL shown / the picture again
int sidecar_wgview_list_shown (void);
void sidecar_wgview_lib (int on);			// FILES aimed at the shown bank's first empty row / back to the rows
int sidecar_wgview_lib_shown (void);
int sidecar_wgview_bank (void);				// the bank shown (the provider fills that one)
void sidecar_wgview_set_bank (int bank);
void sidecar_wgview_pool_page (int on);			// the POOL page (three tabs SAMPLES / TABLES / INSTR) or the machine view
int sidecar_wgview_pool_page_shown (void);
int sidecar_wgview_page_list (void);			// the POOL page's tab (WGV_SAMPLES ..)
void sidecar_wgview_set_page_list (int list);
int sidecar_wgview_list_id (void);			// the list shown: the page's tab, else the voice's machine's (-1 none)
int sidecar_wgview_selecting (void);			// SELECT UNUSED on (the provider fills unused[])
int sidecar_wgview_target (void);			// the code FILES fills (-1 none)
const char *sidecar_wgview_message (void);		// the message row ("" none), the confirm's question while it asks
int sidecar_wgview_asking (void);			// a confirm (P3) or a row's menu is open: 1 confirm, 2 menu

#ifdef __cplusplus
}
#endif

#endif
