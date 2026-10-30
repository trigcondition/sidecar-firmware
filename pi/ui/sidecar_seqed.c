// pi/ui/sidecar_seqed.c - see sidecar_seqed.h. The page is laid out per theme (sidecar_theme.h, docs/UI-THEMES.md):
//   CLASSIC (800x480, below the 52 px title bar, page coordinates):
//     header   y 4..52    SIDECAR SEQ, transport and tempo, the pattern / track, the seq tracks, the page strip, RYTM
//     ruler    y 56..70   grid x 52..692 y 70..350 (rows 14 px), keys x 8..48
//     lane     y 354..424 the velocity lane (DRAW), or the tool's controls (CHORD / STRUM / HUMAN / KEY / SWING)
//     tools    x 700..792 DRAW CHORD STRUM HUMAN KEY SWING (42 px each), SNAP, UNDO / REDO
//   OLED (400x240 dots, left of the navigation column; PAT there is the Rytm view):
//     title    y 0..12    SEQ, the pattern and track, the project, transport and tempo
//     row      y 15..36   the seq tracks, the page strip, SNAP
//     ruler    y 39..47   grid x 27..314 y 48..167 (24 rows of 5 dots, 9 dots a step at 2 pages), keys x 0..23,
//                         pitch up / pages / down x 318..351
//     lane     y 171..214 the velocity lane, or the tool's controls
//     tools    y 218..239 DRAW CHORD STRUM HUMAN KEY SWING, UNDO, REDO
// The grid, keys, ruler, page strip and lane are drawn by code (one LVGL object each, its DRAW_MAIN), culled to the
// area LVGL redraws: an edit invalidates only the rectangles it touches, the play head is its own narrow object, so the
// screen's steady cost while playing is two narrow strips a frame plus the notes that light or go dark under it
// (docs/TOUCH-EDITOR.md "Cost"). Notes speak the Rytm's trig LED colours (core/seq): a note blue, a chord cyan, the
// one playing white (OLED: the panel's blue print a note, the FUNC teal a chord, the held one its light tint); their
// brightness is the velocity.
#include "sidecar_seqed.h"
#include "sidecar_theme.h"
#include "lvgl.h"
#include "src/misc/lv_area_private.h"	// lv_area_intersect: culling to the redrawn area
#include <string.h>

typedef struct
{
	int pg_h;
	int strip_x, strip_y, strip_w, strip_h, cell;
	int tracks_x, tracks_y, tracks_w, tracks_h;
	int rul_y, rul_h, grid_x, grid_y, grid_w, grid_h, keys_x, keys_w;
	int lane_x, lane_y, lane_w, lane_h;
	int tools_x, tools_y, tools_w, tools_h;
	int snap_x, snap_y, snap_w, snap_h;
	int undo_x, undo_y, undo_w, undo_h, redo_x;
	int row_h, row_min, row_max;
	int card_w, card_h;
	int head_w;
} seq_geo_t;

static const seq_geo_t kGeoClassic = {
	.pg_h = 428,
	.strip_x = 430, .strip_y = 4, .strip_w = 222, .strip_h = 48, .cell = 28,
	.tracks_x = 254, .tracks_y = 4, .tracks_w = 168, .tracks_h = 48,
	.rul_y = 56, .rul_h = 14, .grid_x = 52, .grid_y = 70, .grid_w = 640, .grid_h = 280, .keys_x = 8, .keys_w = 40,
	.lane_x = 8, .lane_y = 354, .lane_w = 684, .lane_h = 70,
	.tools_x = 700, .tools_y = 56, .tools_w = 92, .tools_h = 6 * 42 + 5 * 4,	// (touch2: six tools of 42 px; seqx: seven of 35)
	.snap_x = 700, .snap_y = 56 + 5 * 54 + 4, .snap_w = 92, .snap_h = 44,
	.undo_x = 700, .undo_y = 354 + 70 - 48, .undo_w = 44, .undo_h = 48, .redo_x = 748,
	.row_h = 14, .row_min = 8, .row_max = 28,
	.card_w = 260, .card_h = 54, .head_w = 2,
};

static const seq_geo_t kGeoOled = {
	.pg_h = 240,
	.strip_x = 117, .strip_y = 15, .strip_w = 160, .strip_h = 22, .cell = 20,
	.tracks_x = 0, .tracks_y = 15, .tracks_w = 111, .tracks_h = 22,
	.rul_y = 39, .rul_h = 9, .grid_x = 27, .grid_y = 48, .grid_w = 288, .grid_h = 120, .keys_x = 0, .keys_w = 24,
	.lane_x = 0, .lane_y = 171, .lane_w = 352, .lane_h = 44,
	.tools_x = 0, .tools_y = 218, .tools_w = 243, .tools_h = 22,
	.snap_x = 283, .snap_y = 15, .snap_w = 69, .snap_h = 22,
	.undo_x = 249, .undo_y = 218, .undo_w = 50, .undo_h = 22, .redo_x = 302,
	.row_h = 5, .row_min = 4, .row_max = 15,
	.card_w = 172, .card_h = 36, .head_w = 1,
};

static int oled (void) { return TH->id == THEME_OLED; }

// the table now: the theme's 800x480 one, grown by what a 1024x600 screen adds (TH->dx, dy: geo_size)
static seq_geo_t s_Geo;
static const seq_geo_t *G = &kGeoClassic;

// 1024x600 (OLED 512x300 dots, CLASSIC 1024x600 px): the roll gets the extra width (whole dots a step: 12 instead of 9
// at two pages; CLASSIC 27 instead of 20) and two thirds of the extra height (32 rows of 5 dots instead of 24), the lane
// the other third; the header row, the tools and the right-hand keys move with the edges.
static void geo_size (const seq_geo_t *b)
{
	seq_geo_t g = *b;
	const int dx = TH->dx, dy = TH->dy, gx = oled () ? dx * 6 / 7 : dx;	// OLED: 96 of the 112 dots (12 a step)
	g.pg_h += dy;
	g.grid_w += gx;
	g.grid_h += dy * 2 / 3;
	g.lane_y += dy * 2 / 3;
	g.lane_h += dy - dy * 2 / 3;
	g.lane_w += dx;
	if (oled ())
	{
		g.tracks_w += dx / 4;
		g.strip_x = g.tracks_x + g.tracks_w + 6;
		g.snap_w += dx / 4;
		g.snap_x = TH->w - 48 - g.snap_w;
		g.strip_w = g.snap_x - 6 - g.strip_x;
		g.cell = g.strip_w / 8;
		g.tools_y += dy;
		g.undo_y += dy;
		g.tools_w += dx * 5 / 7;
		g.undo_x = g.tools_w + 6;
		g.undo_w += dx / 7;
		g.redo_x = g.undo_x + g.undo_w + 3;
	}
	else
	{
		g.tracks_w += dx / 4;
		g.strip_x += dx / 4;
		g.cell += dx / 16;
		g.strip_w = g.cell * 8 - 2;
		g.tools_x += dx;
		g.tools_h += dy / 2;
		g.snap_x += dx;
		g.snap_y += dy / 2;
		g.undo_x += dx;
		g.redo_x += dx;
		g.undo_y = g.lane_y + g.lane_h - g.undo_h;
	}
	s_Geo = g;
	G = &s_Geo;
}
#define GRID_X	(G->grid_x)
#define GRID_Y	(G->grid_y)
#define GRID_W	(G->grid_w)
#define GRID_H	(G->grid_h)
#define KEYS_W	(G->keys_w)
#define LANE_X	(G->lane_x)
#define LANE_H	(G->lane_h)
#define CELL	(G->cell)
#define STEP96		96
#define PAGE96		(16 * STEP96)


static lv_obj_t *s_Page, *s_Grid, *s_Keys, *s_Ruler, *s_Strip, *s_Lane, *s_Head, *s_Card, *s_CardText, *s_CardSub;
static lv_obj_t *s_Title, *s_TitleBar, *s_Where, *s_Tracks, *s_Tools, *s_Snap, *s_Undo, *s_Redo, *s_Empty, *s_Zoom;
static lv_obj_t *s_Panel[SEQED_TOOLS];			// the lane's place for CHORD, STRUM, HUMAN, KEY, SWING, POLY (index = tool)
static lv_obj_t *s_ChordM, *s_InvM, *s_ChStrM, *s_StrumM, *s_DirM, *s_HumTM, *s_HumVM, *s_RootM, *s_ScaleM, *s_PanelInfo[SEQED_TOOLS];
static lv_obj_t *s_SwingM;				// SWING's presets (a value between them: none checked)
static lv_obj_t *s_VoicesM, *s_AllocM, *s_UniM;		// POLY's (seqx E0): VOICES, ALLOCATION, UNISON
static lv_obj_t *s_QntM, *s_Free;			// seqtop KEY: QNT, the seed's FREE / FIX key
static lv_obj_t *s_SpdM, *s_DirM2, *s_RndM, *s_OctM, *s_WghtM;	// seqtop PLAY: speed, direction, R1's mode / octaves / weight
static lv_obj_t *s_RtM, *s_FxM, *s_RndS, *s_CondM, *s_SlideK;	// seqtop STEP: ratchet / arp, FX 1, random notes, the locks'
static int s_KeyV = -1, s_ScaleV = -1;			// the view's key / scale last taken (a new one moves the KEY tool)
static int s_SwingLast = 58;				// the last swing that was not straight (the value key's toggle)
static sidecar_seqed_command_t s_Cmd;
static void *s_CmdParam;
static void (*s_RytmCb) (void);

static sidecar_seq_view_t s_V, s_Pend;			// what is shown (the model plus local guesses), a view held back
static int s_bPend, s_bHave, s_bInit;
static unsigned s_NeGen = ~0u;				// U1: the Rytm's NOTE EDIT counter last followed
static int s_Tool = SEQED_DRAW;
static int s_EditPg = -1;				// rui1051: the view's edit_page last seen
static int s_First, s_Pages = 2, s_Top = 76, s_RowH = 14;	// the window: first page, pages, the top row's pitch, row height
static int s_Snap96 = STEP96;
static int s_SelStep = -1, s_SelPitch = -1;
static int s_Root, s_Scale = 2;				// C minor
static int s_Chd = 7, s_Inv, s_ChStr = 1;		// MIN7, root position, strum index
static int s_StrumI = 3, s_StrumDir, s_HumT = 2, s_HumV = 2;
static int s_LastLen = 72, s_LastVel = 100, s_bLastSet;
static int s_Play = -1;					// x1000 steps
static unsigned s_TouchMs;				// the last touch (follow waits for 3 s after it)
static int s_bMore;					// the chord row shows the second set
// UX review T5, performance-safe touch: while the Rytm plays the page opens in VIEW (the grid selects and scrolls, it
// does not edit); a tap on DRAW edits, and VIEW comes back after kViewIdleMs without an edit. While playing a new note
// or a delete takes a long press (LVGL's, 400 ms); every edit shows a toast ("+ D#4 S5").
enum { kViewIdleMs = 10000, kToastMs = 1500 };
static int s_bLocked;					// VIEW
static unsigned s_EditMs;				// the last edit or DRAW tap
static lv_obj_t *s_Toast;
static lv_timer_t *s_ToastT;
static int playing (void) { return s_V.transport == 1; }
int sidecar_seqed_first_page (void) { return s_First; }

// a gesture
enum { G_NONE, G_NOTE, G_RESIZE, G_EMPTY, G_PAN, G_PINCH, G_LANE };
static struct
{
	int mode, moved, x0, y0, x, y;
	int step, micro, pitch, len, vel;		// the note as it was
	int nstep, nmicro, npitch, nlen, nvel;		// as it is now
	int first0, top0, pages0, rowh0;
	int x2, y2, d0x, d0y, two;			// the second finger
	int longdone;					// T5: a long press made its edit (the release does nothing more)
} g;

static const char *const kName[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
// core/seq's scales (music.cpp; the first five the KEY tool's keys, the rest named in its info line)
static const unsigned short kScaleMask[14] = {0xFFF, 0xAB5, 0x5AD, 0x6AD, 0x4A9, 0x5AB, 0xAD5, 0x6B5, 0x56B, 0x9AD, 0xAAD, 0x295, 0x4E9, 0x555};
static const char *const kScaleWord[14] = {"CHR", "MAJ", "MIN", "DOR", "PENT", "PHR", "LYD", "MIX", "LOC", "HMIN", "MMIN", "PMAJ", "BLUES", "WHOLE"};
static const char *const kScaleMap[] = {"CHR", "MAJ", "MIN", "DOR", "PENT", ""};
static const char *const kScaleName[14] = {"CHROMATIC", "MAJOR", "MINOR", "DORIAN", "MINOR PENT", "PHRYGIAN", "LYDIAN",
					   "MIXOLYDIAN", "LOCRIAN", "HARM MINOR", "MEL MINOR", "MAJOR PENT", "BLUES", "WHOLE TONE"};
static const char *const kQntMap[] = {"OFF", "EDIT", "PLAY", ""};
static const char *const kQntWord[3] = {"off", "edit", "play"};
// PLAY: core/seq's speed ids in the keys' order (slowest first), the directions, R1's modes / weights
static const int kSpdOrder[10] = {1, 2, 3, 7, 4, 9, 0, 8, 5, 6};
static const char *const kSpdMap[] = {"1/8", "1/4", "1/2", "2/3", "3/4", "7/8", "1", "5/4", "3/2", "2", ""};
static const char *const kSpdWord[10] = {"follow", "1/8", "1/4", "1/2", "3/4", "3/2", "2", "2/3", "5/4", "7/8"};	// by id
static const char *const kDir2Map[] = {"FWD", "REV", "PEND", "PING", "RND", "BRWN", ""};
static const char *const kDir2Word[6] = {"fwd", "rev", "pend", "ping", "rnd", "brown"};
static const char *const kRndMap[] = {"OFF", "STEPS", "TRACK", ""};
static const char *const kRndMapO[] = {"OFF", "STP", "TRK", ""};
static const char *const kRndWord[3] = {"off", "steps", "track"};
static const char *const kOctMap[] = {"1", "2", "3", "4", ""};
static const char *const kWghtMap[] = {"FLAT", "ROOT", "CHRD", "LOW", "HIGH", "STEP", ""};
static const char *const kWghtWord[6] = {"flat", "root", "chord", "low", "high", "step"};
// STEP: the selected note's step - its ratchet / arp, FX column 1, random notes, its locks' condition and slide
static const char *const kRtMap[] = {"OFF", "2", "3", "4", "8", "A-UP", "A-UD", "A-RN", ""};
static const char *const kRtMapO[] = {"OFF", "2", "3", "4", "8", "UP", "UD", "?", ""};
static const char *const kRtLine[8] = {"off", "cnt=2,mode=chord,rate=auto", "cnt=3,mode=chord,rate=auto", "cnt=4,mode=chord,rate=auto",
				       "cnt=8,mode=chord,rate=auto", "cnt=0,mode=up,rate=1/4,oct=2", "cnt=0,mode=updn,rate=1/4,oct=2",
				       "cnt=8,mode=rnd,rate=1/4,oct=2"};
static const char *const kFxMap[] = {"---", "OCT+", "OCT-", "RND2", "JMP1", "SKP1", ""};
static const char *const kFxLine[6] = {"off", "OCT:1", "OCT:-1", "RND:2", "JMP:1", "SKP:1"};
static const char *const kCondMap[] = {"---", "50%", "1:2", "FILL", ""};
static const char *const kCondLine[4] = {"---", "50%", "1:2", "fill"};
static const int kCondCode[4] = {0, 44 + 49, 9, 1};		// core/seq's COND codes (50 %: kConds + 49)
static const char *const kRootMap[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", ""};
static const int kChordSet[2][10] = {{1, 2, 5, 6, 7, 3, 4, 8, 9, 12}, {10, 11, 13, 14, 15, 0, 0, 0, 0, 0}};
static const char *s_ChordMap[12];
static const char *const kInvMap[] = {"R", "1", "2", "3", ""};
static const int kChStr[4] = {0, 6, 12, 24};
static const char *const kChStrMap[] = {"0", "6", "12", "24", ""};
static const int kStrum[6] = {0, 3, 6, 12, 24, 48};
static const char *const kStrumMap[] = {"0", "3", "6", "12", "24", "48", ""};
static const char *const kDirMap[] = {"UP", "DOWN", ""};
static const int kHumT[5] = {0, 2, 4, 8, 16};
static const char *const kHumTMap[] = {"0", "2", "4", "8", "16", ""};
static const int kHumV[4] = {0, 6, 12, 24};
static const char *const kHumVMap[] = {"0", "6", "12", "24", ""};
static const char *const kToolMap[] = {"DRAW", "\n", "CHORD", "\n", "STRUM", "\n", "HUMAN", "\n", "KEY", "\n", "SWING", "\n", "POLY",
				       "\n", "PLAY", "\n", "STEP", ""};
static const char *const kToolRow[] = {"DRAW", "CHRD", "STRM", "HUM", "KEY", "SWG", "POLY", "PLAY", "STEP", ""};
// POLY (seqx E0): VOICES (AUTO: the machine's default), the Analog Four's ALLOCATION modes, UNISON's stack
static const int kVoicesV[7] = {0, 1, 2, 3, 4, 6, 8};
static const char *const kVoicesMap[] = {"AUTO", "1", "2", "3", "4", "6", "8", ""};
static const char *const kAllocMap[] = {"REASGN", "ROTATE", "RESET", ""};
static const char *const kAllocMapO[] = {"REA", "ROT", "RES", ""};		// (the OLED's narrow keys: the info line names it)
static const char *const kAllocWord[3] = {"REASSIGN", "ROTATE", "RESET"};
static const char *const kAllocName[3] = {"reassign", "rotate", "reset"};
static const int kUniV[4] = {1, 2, 3, 4};
static const char *const kUniMap[] = {"OFF", "2", "3", "4", ""};
static const char *const kUniMapO[] = {"1", "2", "3", "4", ""};
static const int kSwing[8] = {50, 54, 58, 62, 66, 70, 75, 80};
static const char *const kSwingMap[] = {"50", "54", "58", "62", "66", "70", "75", "80", ""};
static int swing_now (void) { return s_V.swing > 50 && s_V.swing <= 80 ? s_V.swing : 50; }

static void command (const char *c)
{
	if (s_Cmd) s_Cmd (c, s_CmdParam);
}

static void set_text (lv_obj_t *l, const char *t)
{
	if (l) th_label_set (l, t);
}

static int clampi (int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int iabs (int v) { return v < 0 ? -v : v; }		// no libc abs on the Pi's Circle side
static int in_scale (int pitch) { return (kScaleMask[s_Scale] >> ((pitch - s_Root + 120) % 12)) & 1; }
static int black_key (int pitch) { return (0x54A >> (pitch % 12)) & 1; }
static int npages (void) { return s_V.npages > 0 ? s_V.npages : 1; }
static int rows (void) { return GRID_H / s_RowH; }

// time (1/96 step from the pattern's start) <-> x in the grid object (0..GRID_W)
static int x_of (int pos96)
{
	long d = (long) (pos96 - s_First * PAGE96) * GRID_W;
	long q = (long) PAGE96 * s_Pages;
	return (int) (d >= 0 ? d / q : -((-d + q - 1) / q));
}
static int pos_of (int x) { return s_First * PAGE96 + (int) ((long) x * PAGE96 * s_Pages / GRID_W); }
static int y_of (int pitch) { return (s_Top - pitch) * s_RowH; }
static int pitch_of (int y) { return s_Top - (y < 0 ? -1 - (-y - 1) / s_RowH : y / s_RowH); }
static int snap_near (int pos) { int u = s_Snap96; return pos >= 0 ? (pos + u / 2) / u * u : -((-pos + u / 2) / u * u); }
static int snap_down (int pos) { int u = s_Snap96; return pos >= 0 ? pos / u * u : -((-pos + u - 1) / u * u); }

static void note_name (int pitch, char *b, int cap) { lv_snprintf (b, cap, "%s%d", kName[pitch % 12], pitch / 12 - 1); }

static int play96 (void) { return s_Play < 0 ? -1 : (int) ((long) s_Play * STEP96 / 1000); }

// the notes a step holds (a chord: 2 or more)
static int notes_at (int step)
{
	int n = 0;
	for (int i = 0; i < s_V.n; i++) n += s_V.note[i].step == step;
	return n;
}

// A note's colour. CLASSIC: green by velocity (dark 1 .. light 127). OLED: the trig LEDs' language - blue a note, cyan
// a chord in the trig LEDs' language, recoloured with the panel's: blue print a note, FUNC teal a chord, brightness the
// velocity; white while it plays, the teal's tint held (selected), grey muted.
static uint32_t note_rgb (int step, int micro, int len, int vel, int muted, int sel)
{
	int p = play96 (), on = step * STEP96 + micro;
	int lit = p >= 0 && p >= on && p < on + len;
	if (oled ())
	{
		if (muted) return TH->muted;
		if (sel) return TH->accent_hi;
		if (lit) return 0xFFFFFF;
		return notes_at (step) > 1 ? th_oled_accent (vel) : th_oled_secondary (vel);
	}
	if (muted) return TH->muted;
	uint32_t c = th_mix (0x16502A, TH->accent_hi, clampi (vel * 256 / 127, 0, 256));
	return lit ? th_mix (c, 0xFFFFFF, 150) : c;
}

static int find_note (int step, int pitch)
{
	for (int i = 0; i < s_V.n; i++)
		if (s_V.note[i].step == step && s_V.note[i].pitch == pitch) return i;
	return -1;
}

// The note's rectangle in the grid object's coordinates (x2 inclusive)
static void note_rect (int step, int micro, int pitch, int len, lv_area_t *a)
{
	int p = step * STEP96 + micro;
	a->x1 = x_of (p);
	a->x2 = x_of (p + len) - (oled () ? 2 : 1);		// OLED: a dot dark between two notes in a row
	if (a->x2 < a->x1 + 2) a->x2 = a->x1 + 2;
	a->y1 = y_of (pitch) + (oled () ? 0 : 1);
	a->y2 = y_of (pitch) + s_RowH - 2;
}

static void inval_rect (lv_obj_t *o, int x1, int y1, int x2, int y2)
{
	lv_area_t c, a;
	lv_obj_get_coords (o, &c);
	a.x1 = c.x1 + x1; a.y1 = c.y1 + y1; a.x2 = c.x1 + x2; a.y2 = c.y1 + y2;
	if (lv_area_intersect (&a, &a, &c)) lv_obj_invalidate_area (o, &a);
}

// what a note covers on screen: its bar, its micro stem back to the grid line, its velocity bar
static void inval_note (int step, int micro, int pitch, int len)
{
	lv_area_t a;
	note_rect (step, micro, pitch, len, &a);
	int gx = x_of (step * STEP96), m = oled () ? 2 : 4;
	int x1 = a.x1 < gx ? a.x1 : gx, x2 = a.x2 > gx ? a.x2 : gx;
	inval_rect (s_Grid, x1 - m, a.y1 - m, x2 + m, a.y2 + m);
	inval_rect (s_Lane, x1 - m + GRID_X - LANE_X, 0, x1 + 3 * m + GRID_X - LANE_X, LANE_H - 1);
}

static void inval_all (void)
{
	lv_obj_invalidate (s_Grid);
	lv_obj_invalidate (s_Keys);
	lv_obj_invalidate (s_Ruler);
	lv_obj_invalidate (s_Strip);
	lv_obj_invalidate (s_Lane);
}

// ---- drawing

#define fill th_fill
#define frame th_frame
#define text th_text

static void draw_note (lv_layer_t *l, const lv_area_t *c, int step, int micro, int pitch, int len, int vel, int muted,
		       int sel, int ghost)
{
	lv_area_t a;
	note_rect (step, micro, pitch, len, &a);
	if (a.x2 < 0 || a.x1 >= GRID_W || a.y2 < 0 || a.y1 >= GRID_H) return;
	uint32_t col = note_rgb (step, micro, len, vel, muted, sel);
	int gx = x_of (step * STEP96), cy = (a.y1 + a.y2) / 2, r = oled () ? 0 : 2;
	if (ghost)
	{
		frame (l, c, a.x1, a.y1, a.x2, a.y2, oled () ? DOT_DOTTED : TH->win_dim, 1, r);
		return;
	}
	if (micro != 0 && gx >= 0 && gx < GRID_W)		// micro-timing: a stem from the step's line to the note
	{
		int s1 = micro > 0 ? gx : a.x2, s2 = micro > 0 ? a.x1 : gx;
		if (s2 > s1) fill (l, c, s1, cy, s2, cy, col, oled () ? LV_OPA_COVER : LV_OPA_70, 0);
		if (oled ()) fill (l, c, gx, a.y1, gx, a.y2, col, LV_OPA_COVER, 0);
		else fill (l, c, gx, cy - 2, gx, cy + 2, col, LV_OPA_COVER, 0);
	}
	fill (l, c, a.x1, a.y1, a.x2, a.y2, col, LV_OPA_COVER, r);
	if (oled ())
	{
		if (sel)						// held: a white frame, the resize grip a dark column
		{
			frame (l, c, a.x1 - 1, a.y1 - 1, a.x2 + 1, a.y2 + 1, 0xFFFFFF, 1, 0);
			if (a.x2 - a.x1 >= 6) fill (l, c, a.x2 - 2, a.y1, a.x2 - 2, a.y2, 0x000000, LV_OPA_COVER, 0);
		}
		if (s_RowH >= 8 && a.x2 - a.x1 >= 20)		// the name in the micro face (7 rows) on a bar of 7 rows or more (T4)
		{
			char b[8];
			note_name (pitch, b, sizeof b);
			int bh = a.y2 - a.y1 + 1;
			text (l, c, a.x1 + 2, bh <= 8 ? a.y1 - 1 : a.y1 + (bh - 9) / 2, a.x2 - a.x1 - 2, TH->f_micro, 0x000000, LV_TEXT_ALIGN_LEFT, b);
		}
		return;
	}
	if (a.x2 - a.x1 >= 6) fill (l, c, a.x1, a.y1, a.x1 + 1, a.y2, 0xE8E8E8, LV_OPA_40, 0);	// the onset
	if (sel)
	{
		frame (l, c, a.x1 - 1, a.y1 - 1, a.x2 + 1, a.y2 + 1, 0xE8E8E8, 2, 3);
		fill (l, c, a.x2 - 3, a.y1 + 2, a.x2 - 2, a.y2 - 2, 0x101010, LV_OPA_COVER, 0);	// the resize grip
	}
	if (s_RowH >= 14 && a.x2 - a.x1 >= 34)
	{
		char b[8];
		note_name (pitch, b, sizeof b);
		text (l, c, a.x1 + 5, a.y1 + (a.y2 - a.y1 + 1 - 14) / 2, a.x2 - a.x1 - 6, &lv_font_montserrat_12, 0x101010,
		      LV_TEXT_ALIGN_LEFT, b);
	}
}

static void grid_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	int nr = rows () + 1;
	for (int r = 0; r < nr; r++)			// the rows: the scale, its root
	{
		int p = s_Top - r, y = r * s_RowH;
		if (p < 0) break;
		uint32_t col = p % 12 == s_Root % 12 && s_Scale ? TH->row_root : in_scale (p) ? TH->row_in : TH->row_out;
		if (!s_Scale) col = black_key (p) ? TH->row_out : TH->row_in;
		fill (l, &c, 0, y, GRID_W - 1, y + s_RowH - 1, col, LV_OPA_COVER, 0);
		if (p % 12 == 0) fill (l, &c, 0, y + s_RowH - 1, GRID_W - 1, y + s_RowH - 1, TH->line_beat, LV_OPA_COVER, 0);
	}
	int px = GRID_W / (16 * s_Pages);		// pixels a step
	int len = s_V.length > 0 ? s_V.length : 16 * npages ();
	int xe = x_of (len * STEP96);			// past the track's length: shaded, under the step lines
	if (xe < GRID_W)
	{
		if (oled ()) fill (l, &c, xe < 0 ? 0 : xe, 0, GRID_W - 1, GRID_H - 1, 0x161616, LV_OPA_COVER, 0);	// past the end: darker, even
		else fill (l, &c, xe < 0 ? 0 : xe, 0, GRID_W - 1, GRID_H - 1, 0x000000, LV_OPA_50, 0);
	}
	// (the lines over it: every visible page keeps its grid - the hardware report, 0.3.0: stopped, the length can read
	// one page while the window kept more from playing, and the OLED's opaque shade hid the lines past page 1)
	for (int s = s_First * 16; s <= (s_First + s_Pages) * 16; s++)
	{
		int x = x_of (s * STEP96);
		if (x >= GRID_W) x = GRID_W - 1;
		uint32_t col = s % 16 == 0 ? TH->line_page : s % 4 == 0 ? TH->line_beat : TH->line_step;
		if (s % 4 && px < (oled () ? 4 : 8)) continue;
		if (oled () && s % 16) col = s % 4 ? TH->line_step : DOT_DOTTED;	// beats dotted, pages solid
		fill (l, &c, x, 0, x, GRID_H - 1, col, LV_OPA_COVER, 0);
	}
	for (int i = 0; i < s_V.n; i++)
	{
		const sidecar_seq_note_t *n = &s_V.note[i];
		int sel = n->step == s_SelStep && n->pitch == s_SelPitch;
		if (sel && g.moved && (g.mode == G_NOTE || g.mode == G_RESIZE))
		{
			draw_note (l, &c, n->step, n->micro, n->pitch, n->len, n->vel, 0, 0, 1);	// where it was
			draw_note (l, &c, g.nstep, g.nmicro, g.npitch, g.nlen, n->vel, n->flags & 1, 1, 0);
			continue;
		}
		draw_note (l, &c, n->step, n->micro, n->pitch, n->len, n->vel, n->flags & 1, sel, 0);
		if (n->flags & 2)				// U1: in the Rytm's NOTE EDIT selection - a second frame, outside
		{
			lv_area_t a;
			note_rect (n->step, n->micro, n->pitch, n->len, &a);
			int m = oled () ? 2 : 3;
			frame (l, &c, a.x1 - m, a.y1 - m, a.x2 + m, a.y2 + m, oled () ? TH->accent_hi : 0xF0C040, 1, oled () ? 0 : 4);
		}
	}
}

static void keys_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Keys, &c);
	int nr = rows () + 1;
	for (int r = 0; r < nr; r++)
	{
		int p = s_Top - r, y = r * s_RowH;
		if (y >= GRID_H || p < 0) break;
		int y2 = y + s_RowH - 1 < GRID_H - 1 ? y + s_RowH - 1 : GRID_H - 1;
		if (oled ())
		{
			if (black_key (p)) fill (l, &c, 0, y, 14, y2, TH->key_black, LV_OPA_COVER, 0);	// a black key's stub
			if (p % 12 == 0 || p % 12 == 5) fill (l, &c, 0, y2, KEYS_W - 5, y2, TH->faint, LV_OPA_COVER, 0);	// B|C, E|F
			if (s_Scale && in_scale (p))
				fill (l, &c, KEYS_W - 3, y, KEYS_W - 1, y2 - 1, p % 12 == s_Root % 12 ? TH->accent : TH->dim, LV_OPA_COVER, 0);
			continue;
		}
		fill (l, &c, 0, y, KEYS_W - 1, y2 - 1, black_key (p) ? TH->key_black : TH->key_white, LV_OPA_COVER, 0);
		if (s_Scale && in_scale (p))
			fill (l, &c, KEYS_W - 4, y + 2, KEYS_W - 2, y2 - 3, p % 12 == s_Root % 12 ? TH->accent : 0x0D3B1C, LV_OPA_COVER, 1);
		if (p % 12 == 0 || (p % 12 == s_Root % 12 && s_RowH >= 12))
		{
			char b[8];
			note_name (p, b, sizeof b);
			text (l, &c, 3, y + (s_RowH - 14) / 2, KEYS_W - 8, &lv_font_montserrat_12,
			      p % 12 == s_Root % 12 && s_Scale ? TH->accent_hi : TH->win_dim, LV_TEXT_ALIGN_LEFT, b);
		}
	}
	if (oled ())					// the C labels last: micro text on a black patch over the 5-dot rows
		for (int r = 0; r < nr; r++)
		{
			int p = s_Top - r, y = r * s_RowH;
			if (y >= GRID_H || p < 0) break;
			if (p % 12) continue;
			char b[8];
			note_name (p, b, sizeof b);
			int ty = y + s_RowH - 9, tw = th_text_width (TH->f_micro, b);
			fill (l, &c, 0, ty + 1, tw + 1, ty + 8, 0x000000, LV_OPA_COVER, 0);
			text (l, &c, 1, ty, KEYS_W - 3, TH->f_micro, p % 12 == s_Root % 12 && s_Scale ? TH->accent_hi : TH->ink, LV_TEXT_ALIGN_LEFT, b);
		}
}

static void ruler_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Ruler, &c);
	int px = GRID_W / (16 * s_Pages);
	const lv_font_t *f = oled () ? TH->f_micro : &lv_font_montserrat_12;
	for (int s = s_First * 16; s < (s_First + s_Pages) * 16; s += 4)
	{
		int x = x_of (s * STEP96);
		char b[8];
		if (s % 16 == 0) lv_snprintf (b, sizeof b, "P%d", s / 16 + 1);
		else if (px >= (oled () ? 4 : 8) || (s % 8 == 0 && px >= (oled () ? 2 : 0))) lv_snprintf (b, sizeof b, "%d", s % 16 + 1);
		else continue;
		uint32_t ink = s % 16 ? (oled () ? TH->dim : TH->dim) : TH->ink;
		if (oled ()) fill (l, &c, x, s % 16 ? 6 : 0, x, G->rul_h - 1, s % 16 ? TH->faint : TH->line_page, LV_OPA_COVER, 0);
		else fill (l, &c, x, s % 16 ? 9 : 0, x, G->rul_h - 1, s % 16 ? 0x6C6E72 : 0x3A3C40, LV_OPA_COVER, 0);
		text (l, &c, x + 2, -1, 30, f, ink, LV_TEXT_ALIGN_LEFT, b);
	}
	// T4 / S3: a step with a trig condition (FILL, A:B, a probability ..) a mark at the ruler's foot
	for (int s = s_First * 16; s < (s_First + s_Pages) * 16 && s < SEQED_STEPS; s++)
	{
		if (!s_V.cond[s]) continue;
		int x0 = x_of (s * STEP96) + 1, x1 = x_of ((s + 1) * STEP96) - 2;
		if (x1 > x0 + (oled () ? 4 : 8)) x1 = x0 + (oled () ? 4 : 8);
		fill (l, &c, x0, G->rul_h - (oled () ? 2 : 4), x1, G->rul_h - 1, oled () ? TH->accent : TH->warn, LV_OPA_COVER, 0);
	}
}

static void strip_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Strip, &c);
	int playing = s_Play >= 0 ? s_Play / 16000 : -1;
	for (int p = 0; p < 8; p++)
	{
		int x = p * CELL, on = p >= s_First && p < s_First + s_Pages, there = p < npages ();
		lv_area_t a = {c.x1 + x, c.y1, c.x1 + x + CELL - (oled () ? 2 : 3), c.y1 + G->strip_h - 1};
		char b[4];
		lv_snprintf (b, sizeof b, "%d", p + 1);
		// a trig key a page: the shown ones checked, the playing one's LED white (core/seq's PAGE LEDs)
		th_paint_key (l, &a, b, 0, oled () ? TH->f_body : &lv_font_montserrat_16, TH_KEY_TRIG, on, 0, !there,
			      p == playing ? LED_PLAY : there ? 0 : 0);
	}
}

static void lane_draw (lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer (e);
	lv_area_t c;
	lv_obj_get_coords (s_Lane, &c);
	const int ox = GRID_X - LANE_X, top = oled () ? 4 : 8, h = LANE_H - (oled () ? 7 : 14);
	const lv_font_t *f = oled () ? TH->f_micro : &lv_font_montserrat_12;
	if (oled ()) frame (l, &c, 0, 0, G->lane_w - 1, LANE_H - 1, DOT_DOTTED, 1, 0);
	fill (l, &c, ox, top + h, ox + GRID_W - 1, top + h, oled () ? TH->dim : 0x383838, LV_OPA_COVER, 0);
	fill (l, &c, ox, top + h / 2, ox + GRID_W - 1, top + h / 2, oled () ? DOT_DOTTED : 0x222222, LV_OPA_COVER, 0);
	text (l, &c, oled () ? 3 : 0, oled () ? 3 : 4, KEYS_W, f, oled () ? TH->dim : TH->win_dim, LV_TEXT_ALIGN_LEFT, "VEL");
	// seq-ux: the lock lane over the velocities: a step with parameter locks (the Rytm's hold trig + SRC / SMPL / FX
	// + a knob) a bar along its width, brighter with more locks (a heatmap)
	for (int s = s_First * 16; s < (s_First + s_Pages) * 16 && s < SEQED_STEPS; s++)
	{
		int nl = s_V.locks[s];
		if (!nl) continue;
		int x0 = x_of (s * STEP96), x1 = x_of ((s + 1) * STEP96) - 2;
		if (x0 < 0 || x0 >= GRID_W) continue;
		if (x1 < x0) x1 = x0;
		int k = nl > SEQED_LOCKS ? SEQED_LOCKS : nl;
		uint32_t col = oled () ? (k >= 4 ? TH->ink : TH->dim) : (uint32_t) ((0x60 + 0x9F * k / SEQED_LOCKS) << 16 | 0x30 << 8 | (0x60 + 0x9F * k / SEQED_LOCKS));
		fill (l, &c, ox + x0, 0, ox + x1, oled () ? 2 : 4, col, LV_OPA_COVER, 0);
	}
	// seqtop: a step's extras, one mark each (the most telling): R a ratchet / arp, F an FX command, ~ random notes,
	// ? a lock's condition, / a sliding lock
	for (int s = s_First * 16; s < (s_First + s_Pages) * 16 && s < SEQED_STEPS; s++)
	{
		int x = s_V.xstep[s];
		if (!x) continue;
		int x0 = x_of (s * STEP96);
		if (x0 < 0 || x0 >= GRID_W) continue;
		const char *m = x & 2 ? "R" : x & 4 ? "F" : x & 1 ? "~" : x & 8 ? "?" : "/";
		text (l, &c, ox + x0 + 1, oled () ? 3 : 6, oled () ? 8 : 12, f, oled () ? TH->ink : TH->accent, LV_TEXT_ALIGN_LEFT, m);
	}
	for (int s = s_First * 16; s <= (s_First + s_Pages) * 16; s += 16)
	{
		int x = x_of (s * STEP96);
		if (x >= GRID_W) x = GRID_W - 1;
		fill (l, &c, ox + x, top, ox + x, top + h, TH->line_page, LV_OPA_COVER, 0);
	}
	for (int i = 0; i < s_V.n; i++)
	{
		const sidecar_seq_note_t *n = &s_V.note[i];
		int sel = n->step == s_SelStep && n->pitch == s_SelPitch;
		int vel = sel && g.mode == G_LANE ? g.nvel : n->vel;
		int x = x_of (n->step * STEP96 + n->micro);
		if (x < 0 || x >= GRID_W) continue;
		int bh = vel * h / 127;
		uint32_t col = sel ? (oled () ? TH->accent_hi : 0xE8E8E8) : note_rgb (n->step, n->micro, n->len, vel, n->flags & 1, 0);
		fill (l, &c, ox + x, top + h - bh, ox + x + (oled () ? 2 : 3), top + h - 1, col, LV_OPA_COVER, oled () ? 0 : 1);
	}
	if (s_SelStep >= 0)
	{
		int i = find_note (s_SelStep, s_SelPitch);
		if (i >= 0)
		{
			char b[8];
			lv_snprintf (b, sizeof b, "%d", g.mode == G_LANE ? g.nvel : s_V.note[i].vel);
			text (l, &c, oled () ? 3 : 0, oled () ? 17 : 24, KEYS_W, oled () ? TH->f_body : &lv_font_montserrat_16,
			      oled () ? TH->ink : TH->win_ink, LV_TEXT_ALIGN_LEFT, b);
		}
	}
}

// ---- the selection's card

static void card_update (void)
{
	int i = s_SelStep >= 0 ? find_note (s_SelStep, s_SelPitch) : -1;
	if (i < 0 || s_Tool != SEQED_DRAW)
	{
		lv_obj_add_flag (s_Card, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	const sidecar_seq_note_t *n = &s_V.note[i];
	int moving = g.moved && (g.mode == G_NOTE || g.mode == G_RESIZE);
	int st = moving ? g.nstep : n->step, mi = moving ? g.nmicro : n->micro, pi = moving ? g.npitch : n->pitch;
	int ln = moving ? g.nlen : n->len;
	char nm[8], b[64];
	note_name (pi, nm, sizeof nm);
	lv_snprintf (b, sizeof b, "%s   VEL %d", nm, n->vel);
	set_text (s_CardText, b);
	lv_snprintf (b, sizeof b, oled () ? "%d.%d  L%d.%02d  %s%d/96" : "STEP %d.%d  LEN %d.%02d  MICRO %s%d/96", st / 16 + 1,
		     st % 16 + 1, ln / 96, ln % 96 * 100 / 96, mi > 0 ? "+" : "", mi);
	set_text (s_CardSub, b);
	lv_area_t a, o;
	note_rect (st, mi, pi, ln, &a);
	note_rect (n->step, n->micro, n->pitch, n->len, &o);	// where it was (the outline while it moves)
	int top = a.y1 < o.y1 ? a.y1 : o.y1, bot = a.y2 > o.y2 ? a.y2 : o.y2;
	int gap = oled () ? 4 : 8;
	int x = GRID_X + a.x1 - (oled () ? 12 : 20), y = GRID_Y + top - G->card_h - gap;
	if (y < GRID_Y + 2) y = GRID_Y + bot + gap;
	x = clampi (x, GRID_X + (oled () ? 0 : 2), GRID_X + GRID_W - G->card_w - (oled () ? 0 : 2));
	lv_obj_set_pos (s_Card, x, y);
	lv_obj_remove_flag (s_Card, LV_OBJ_FLAG_HIDDEN);
}

static void select_note (int step, int pitch)
{
	int i = s_SelStep >= 0 ? find_note (s_SelStep, s_SelPitch) : -1;
	if (i >= 0) inval_note (s_V.note[i].step, s_V.note[i].micro, s_V.note[i].pitch, s_V.note[i].len);
	s_SelStep = step;
	s_SelPitch = pitch;
	i = step >= 0 ? find_note (step, pitch) : -1;
	if (i >= 0) inval_note (s_V.note[i].step, s_V.note[i].micro, s_V.note[i].pitch, s_V.note[i].len);
	card_update ();
}

// ---- local guesses (the model's answer replaces them)

static void local_add (int step, int micro, int pitch, int vel, int len)
{
	int i = find_note (step, pitch);
	if (i < 0)
	{
		if (s_V.n >= SEQED_NOTES) return;
		i = s_V.n++;
	}
	sidecar_seq_note_t *n = &s_V.note[i];
	n->step = (unsigned short) step; n->micro = (signed char) micro; n->pitch = (unsigned char) pitch;
	n->vel = (unsigned char) vel; n->len = (unsigned short) len; n->flags = 0;
	inval_note (step, micro, pitch, len);
}

static void local_del (int i)
{
	sidecar_seq_note_t *n = &s_V.note[i];
	inval_note (n->step, n->micro, n->pitch, n->len);
	s_V.note[i] = s_V.note[--s_V.n];
}

static void apply_pending (void)
{
	if (!s_bPend) return;
	s_bPend = 0;
	sidecar_seqed_view (&s_Pend);
}

static void header_update (void);
static void panel_info (void);

// ---- T5: the toast, VIEW
static void toast_end (lv_timer_t *t) { (void) t; if (s_Toast) lv_obj_add_flag (s_Toast, LV_OBJ_FLAG_HIDDEN); s_ToastT = 0; }
static void toast (const char *t)
{
	if (!s_Toast) return;
	th_label_set (s_Toast, t);
	lv_obj_remove_flag (s_Toast, LV_OBJ_FLAG_HIDDEN);
	if (s_ToastT) lv_timer_reset (s_ToastT);
	else { s_ToastT = lv_timer_create (toast_end, kToastMs, 0); lv_timer_set_repeat_count (s_ToastT, 1); }
}
static void lock_set (int on)
{
	if (s_bLocked == on && s_bInit) return;
	s_bLocked = on;
	if (s_Tools) th_key_text (th_seg_item (s_Tools, SEQED_DRAW), on ? "VIEW" : "DRAW");
	if (!on) s_EditMs = lv_tick_get ();
}
static void edited (void) { s_EditMs = lv_tick_get (); }

// "seqed=OP:T:a:b..." with the first n of a..e; the edit's toast
static void cmd_note (const char *op, int a, int b, int c, int d, int e, int n)
{
	char l[96], nm[8];
	int v[5] = {a, b, c, d, e}, k = lv_snprintf (l, sizeof l, "seqed=%s:%d", op, s_V.track);
	for (int i = 0; i < n && k < (int) sizeof l; i++) k += lv_snprintf (l + k, sizeof l - k, ":%d", v[i]);
	command (l);
	edited ();
	if (!strcmp (op, "add")) { note_name (b, nm, sizeof nm); lv_snprintf (l, sizeof l, "+ %s S%d", nm, a + 1); }
	else if (!strcmp (op, "del")) { note_name (b, nm, sizeof nm); lv_snprintf (l, sizeof l, "- %s S%d", nm, a + 1); }
	else if (!strcmp (op, "move")) { note_name (d, nm, sizeof nm); lv_snprintf (l, sizeof l, "> %s S%d", nm, c + 1); }
	else if (!strcmp (op, "len")) { note_name (b, nm, sizeof nm); lv_snprintf (l, sizeof l, "LEN %s S%d", nm, a + 1); }
	else if (!strcmp (op, "vel")) { note_name (b, nm, sizeof nm); lv_snprintf (l, sizeof l, "VEL %s %d", nm, c); }
	else return;
	toast (l);
}

// the steps a step tool acts on: the tapped step, or with APPLY the visible window
static void step_tool (int s0, int s1)
{
	char l[96];
	if (s_V.track < 0) return;
	if (s_Tool == SEQED_STRUM)
		lv_snprintf (l, sizeof l, "seqed=strum:%d:%d:%d:%d", s_V.track, s0, s1, kStrum[s_StrumI] * (s_StrumDir ? -1 : 1));
	else
		lv_snprintf (l, sizeof l, "seqed=human:%d:%d:%d:%d:%d:%u", s_V.track, s0, s1, kHumT[s_HumT], kHumV[s_HumV],
			     (unsigned) lv_tick_get () & 0xFFFF);
	command (l);
}

// ---- touch on the grid

static void point (lv_obj_t *o, int *x, int *y)
{
	lv_point_t p;
	lv_indev_get_point (lv_indev_active (), &p);
	lv_area_t c;
	lv_obj_get_coords (o, &c);
	*x = p.x - c.x1;
	*y = p.y - c.y1;
}

static int hit (int x, int y, int *resize)
{
	int m = oled () ? 2 : 4;				// a finger's slack: 4 px, two dots
	for (int i = s_V.n - 1; i >= 0; i--)
	{
		const sidecar_seq_note_t *n = &s_V.note[i];
		lv_area_t a;
		note_rect (n->step, n->micro, n->pitch, n->len, &a);
		if (y < a.y1 - 1 || y > a.y2 + 1 || x < a.x1 - m || x > a.x2 + m) continue;
		int w = a.x2 - a.x1, edge = oled () ? (w / 4 > 5 ? 5 : w / 4 < 2 ? 2 : w / 4) : (w / 4 > 10 ? 10 : w / 4 < 4 ? 4 : w / 4);
		*resize = x >= a.x2 - edge && w >= (oled () ? 6 : 12);
		return i;
	}
	return -1;
}

static void window (int first, int pages, int top)
{
	int np = npages ();
	pages = clampi (pages, 1, 8);
	first = clampi (first, 0, np > pages ? np - pages : 0);
	top = clampi (top, rows () - 1, 127);
	if (first == s_First && pages == s_Pages && top == s_Top) return;
	s_First = first;
	s_Pages = pages;
	s_Top = top;
	inval_all ();
	sidecar_seqed_playhead (s_Play);
	card_update ();
	header_update ();
	panel_info ();
}

static void grid_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	int x, y;
	point (s_Grid, &x, &y);
	s_TouchMs = lv_tick_get ();
	if (code == LV_EVENT_PRESSED)
	{
		memset (&g, 0, sizeof g);
		g.x0 = g.x = x; g.y0 = g.y = y;
		g.first0 = s_First; g.top0 = s_Top; g.pages0 = s_Pages; g.rowh0 = s_RowH;
		int rs = 0, i = hit (x, y, &rs);
		if (i >= 0 && s_Tool == SEQED_DRAW)		// the selected note's right edge resizes, elsewhere a note moves
		{
			const sidecar_seq_note_t *n = &s_V.note[i];
			g.mode = rs && n->step == s_SelStep && n->pitch == s_SelPitch ? G_RESIZE : G_NOTE;
			g.step = g.nstep = n->step; g.micro = g.nmicro = n->micro; g.pitch = g.npitch = n->pitch;
			g.len = g.nlen = n->len; g.vel = n->vel;
		}
		else g.mode = G_EMPTY;
		return;
	}
	if (code == LV_EVENT_PRESSING)
	{
		g.x = x; g.y = y;
		if (g.mode == G_PINCH || g.mode == G_NONE) return;
		int dx = x - g.x0, dy = y - g.y0, slop = oled () ? 4 : 8;
		if (!g.moved && dx * dx + dy * dy < slop * slop) return;
		if (s_bLocked && (g.mode == G_NOTE || g.mode == G_RESIZE)) g.mode = G_PAN;	// T5 VIEW: a drag scrolls
		if (g.longdone) return;
		if (!g.moved && g.mode == G_NOTE)		// a drag takes the note along: selected from here on
		{
			select_note (g.step, g.pitch);
		}
		g.moved = 1;
		if (g.mode == G_NOTE || g.mode == G_RESIZE)
		{
			inval_note (g.nstep, g.nmicro, g.npitch, g.nlen);
			int d96 = (int) ((long) dx * PAGE96 * s_Pages / GRID_W);
			if (g.mode == G_NOTE)
			{
				int p = g.step * STEP96 + g.micro + snap_near (d96);	// moved by whole snap units: a strum stays
				int len = s_V.length > 0 ? s_V.length : 16 * npages ();
				p = clampi (p, -STEP96 / 2, len * STEP96 - 1);
				int st = p >= 0 ? (p + STEP96 / 2) / STEP96 : 0;
				if (st >= len) st = len - 1;
				g.nstep = st;
				g.nmicro = clampi (p - st * STEP96, -STEP96, STEP96);
				g.npitch = clampi (g.pitch - (dy >= 0 ? (dy + s_RowH / 2) / s_RowH : -((-dy + s_RowH / 2) / s_RowH)), 0, 127);
			}
			else
			{
				int u = s_Snap96;
				int l = g.len + d96;
				l = u > 1 ? (l + u / 2) / u * u : l;
				g.nlen = clampi (l, u > 1 ? u : 1, 16 * STEP96 * 8);
			}
			inval_note (g.nstep, g.nmicro, g.npitch, g.nlen);
			card_update ();
			return;
		}
		if (g.mode == G_EMPTY) g.mode = G_PAN;
		if (g.mode == G_PAN)
		{
			int pw = GRID_W / s_Pages;
			int dp = dx >= 0 ? (dx + pw / 2) / pw : -((-dx + pw / 2) / pw);
			int dr = dy >= 0 ? (dy + s_RowH / 2) / s_RowH : -((-dy + s_RowH / 2) / s_RowH);
			window (g.first0 - dp, s_Pages, g.top0 + dr);
		}
		return;
	}
	if (code == LV_EVENT_LONG_PRESSED)		// T5: while playing, a new note or a delete takes a long press
	{
		if (g.moved || s_bLocked || !playing () || s_V.track < 0) return;
		if (g.mode == G_EMPTY && s_Tool == SEQED_DRAW)
		{
			int pos = pos_of (g.x0), pitch = clampi (pitch_of (g.y0), 0, 127);
			int len = s_V.length > 0 ? s_V.length : 16 * npages ();
			if (pos / STEP96 < 0 || pos / STEP96 >= len) return;
			int p = snap_down (pos), st = p / STEP96, mi = p - st * STEP96;
			if (!s_bLastSet && s_V.dvel > 0) { s_LastVel = s_V.dvel; s_LastLen = s_V.dlen > 0 ? s_V.dlen : 72; }
			local_add (st, mi, pitch, s_LastVel, s_LastLen);
			select_note (st, pitch);
			cmd_note ("add", st, pitch, mi, s_LastVel, s_LastLen, 5);
			g.longdone = 1;
		}
		else if (g.mode == G_NOTE && g.step == s_SelStep && g.pitch == s_SelPitch)
		{
			int i = find_note (g.step, g.pitch);
			if (i >= 0) { local_del (i); select_note (-1, -1); cmd_note ("del", g.step, g.pitch, 0, 0, 0, 2); }
			g.longdone = 1;
		}
		return;
	}
	if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
	int mode = g.mode, moved = g.moved, longdone = g.longdone;
	g.mode = G_NONE;
	g.moved = 0;
	g.longdone = 0;
	if (longdone) { apply_pending (); card_update (); return; }
	if (code == LV_EVENT_PRESS_LOST || mode == G_PINCH || s_V.track < 0) { apply_pending (); card_update (); return; }
	if ((mode == G_NOTE || mode == G_RESIZE) && moved)
	{
		int i = find_note (g.step, g.pitch);
		if (mode == G_NOTE && i >= 0 && (g.nstep != g.step || g.nmicro != g.micro || g.npitch != g.pitch))
		{
			int vel = s_V.note[i].vel;		// the guess first: the command may bring the model's view at once
			local_del (i);
			local_add (g.nstep, g.nmicro, g.npitch, vel, g.len);
			s_SelStep = g.nstep; s_SelPitch = g.npitch;
			cmd_note ("move", g.step, g.pitch, g.nstep, g.npitch, g.nmicro, 5);
		}
		else if (mode == G_RESIZE && i >= 0 && g.nlen != g.len)
		{
			s_V.note[i].len = (unsigned short) g.nlen;
			s_LastLen = g.nlen;
			inval_note (g.step, g.micro, g.pitch, g.len > g.nlen ? g.len : g.nlen);
			cmd_note ("len", g.step, g.pitch, g.nlen, 0, 0, 3);
		}
		apply_pending ();
		card_update ();
		return;
	}
	if (mode == G_NOTE || mode == G_RESIZE)			// a tap on a note: select it; on the selected one: delete
	{
		int i = find_note (g.step, g.pitch);
		if (i >= 0 && g.step == s_SelStep && g.pitch == s_SelPitch && (s_bLocked || playing ()))
			toast (s_bLocked ? "VIEW: TAP DRAW TO EDIT" : "HOLD TO DELETE (PLAYING)");	// T5
		else if (i >= 0 && g.step == s_SelStep && g.pitch == s_SelPitch)
		{
			local_del (i);
			select_note (-1, -1);
			cmd_note ("del", g.step, g.pitch, 0, 0, 0, 2);
		}
		else
		{
			select_note (g.step, g.pitch);
			if (i >= 0) { s_LastLen = s_V.note[i].len; s_LastVel = s_V.note[i].vel; s_bLastSet = 1; }
		}
		apply_pending ();
		return;
	}
	if (mode != G_EMPTY || moved) { apply_pending (); return; }
	if (s_bLocked || (playing () && s_Tool != SEQED_KEY && s_Tool != SEQED_SWING && s_Tool != SEQED_POLY))	// T5: no edit by a plain tap now
	{
		toast (s_bLocked ? "VIEW: TAP DRAW TO EDIT" : s_Tool == SEQED_DRAW ? "HOLD TO ADD (PLAYING)" : "STOP THE RYTM TO STAMP");
		apply_pending ();
		return;
	}
	int pos = pos_of (x), pitch = clampi (pitch_of (y), 0, 127);
	int len = s_V.length > 0 ? s_V.length : 16 * npages ();
	int step = pos / STEP96;
	if (step < 0 || step >= len) { apply_pending (); return; }
	if (s_Tool == SEQED_DRAW)
	{
		int p = snap_down (pos);
		int st = p / STEP96, mi = p - st * STEP96;
		if (!s_bLastSet && s_V.dvel > 0) { s_LastVel = s_V.dvel; s_LastLen = s_V.dlen > 0 ? s_V.dlen : 72; }
		local_add (st, mi, pitch, s_LastVel, s_LastLen);
		select_note (st, pitch);
		cmd_note ("add", st, pitch, mi, s_LastVel, s_LastLen, 5);
	}
	else if (s_Tool == SEQED_CHORD)
	{
		char l[96];
		if (!s_bLastSet && s_V.dvel > 0) { s_LastVel = s_V.dvel; s_LastLen = s_V.dlen > 0 ? s_V.dlen : 72; }
		lv_snprintf (l, sizeof l, "seqed=chord:%d:%d:%d:%d:%d:%d:%d:%d", s_V.track, step, pitch, s_Chd, s_Inv, s_LastVel,
			     s_LastLen > 2 * STEP96 ? s_LastLen : 2 * STEP96, kChStr[s_ChStr]);	// a chord: 2 steps at least
		command (l);
	}
	else if (s_Tool == SEQED_STRUM || s_Tool == SEQED_HUMAN) step_tool (step, step);
	apply_pending ();
}

// the page strip: a tap shows that page, a drag across pages shows them all (1..8)
static int s_StripA = -1;
static void strip_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	int x, y;
	point (s_Strip, &x, &y);
	int p = clampi (x / CELL, 0, 7);
	if (p >= npages ()) p = npages () - 1;
	s_TouchMs = lv_tick_get ();
	if (code == LV_EVENT_PRESSED) s_StripA = p;
	if (s_StripA < 0) return;
	if (code == LV_EVENT_PRESSING || code == LV_EVENT_RELEASED)
	{
		int a = s_StripA < p ? s_StripA : p, b = s_StripA < p ? p : s_StripA;
		window (a, b - a + 1, s_Top);
	}
	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_StripA = -1;
}

// the keys column: a vertical drag scrolls the pitches
static int s_KeysY0, s_KeysTop0;
static void keys_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	int x, y;
	point (s_Keys, &x, &y);
	if (code == LV_EVENT_PRESSED) { s_KeysY0 = y; s_KeysTop0 = s_Top; }
	else if (code == LV_EVENT_PRESSING)
	{
		int dy = y - s_KeysY0;
		window (s_First, s_Pages, s_KeysTop0 + (dy >= 0 ? (dy + s_RowH / 2) / s_RowH : -((-dy + s_RowH / 2) / s_RowH)));
	}
}

// the velocity lane: the note nearest the finger (the selected one first), dragged up and down
static void lane_cb (lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code (e);
	int x, y;
	point (s_Lane, &x, &y);
	x -= GRID_X - LANE_X;
	const int top = oled () ? 4 : 8, h = LANE_H - (oled () ? 7 : 14);
	if (code == LV_EVENT_PRESSED)
	{
		int best = -1, bd = oled () ? 7 : 14;
		for (int i = 0; i < s_V.n; i++)
		{
			const sidecar_seq_note_t *n = &s_V.note[i];
			int d = iabs (x_of (n->step * STEP96 + n->micro) + 1 - x);
			if (n->step == s_SelStep && n->pitch == s_SelPitch) d -= oled () ? 3 : 4;
			if (d < bd) { bd = d; best = i; }
		}
		if (best < 0) return;
		select_note (s_V.note[best].step, s_V.note[best].pitch);
		g.mode = G_LANE;
		g.step = s_SelStep; g.pitch = s_SelPitch; g.vel = g.nvel = s_V.note[best].vel;
	}
	if (g.mode != G_LANE) return;
	if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING)
	{
		g.nvel = clampi ((top + h - y) * 127 / h, 1, 127);
		lv_obj_invalidate (s_Lane);
		return;
	}
	if (code == LV_EVENT_RELEASED)
	{
		int i = find_note (g.step, g.pitch);
		if (i >= 0 && g.nvel != s_V.note[i].vel)
		{
			s_V.note[i].vel = (unsigned char) g.nvel;
			s_LastVel = g.nvel;
			s_bLastSet = 1;
			inval_note (g.step, s_V.note[i].micro, g.pitch, s_V.note[i].len);
			cmd_note ("vel", g.step, g.pitch, g.nvel, 0, 0, 3);
		}
	}
	g.mode = G_NONE;
	card_update ();
	apply_pending ();
}

// ---- the controls

static void ctl_cb (lv_obj_t *m, int i, void *ud);
static lv_obj_t *seg (lv_obj_t *parent, int x, int y, int w, int h, const char *const *map, int sel, int kind)
{
	lv_obj_t *m = th_seg (parent, x, y, w, h, map, kind, ctl_cb, 0);
	if (!oled ()) th_seg_font (m, &lv_font_montserrat_14);
	if (sel >= 0) th_seg_set (m, sel);
	return m;
}

static lv_obj_t *button (lv_obj_t *parent, int x, int y, int w, int h, const char *t, int kind, th_cb_t cb, const lv_font_t *f)
{
	lv_obj_t *b = th_key (parent, x, y, w, h, t, kind, cb, 0);
	if (f) th_key_font (b, f);
	return b;
}

static lv_obj_t *label (lv_obj_t *p, int x, int y, const lv_font_t *f, uint32_t col, const char *t)
{
	return th_label (p, x, y, f, col, t);
}

static lv_obj_t *drawn (lv_obj_t *p, int x, int y, int w, int h, lv_event_cb_t draw, lv_event_cb_t touch, uint32_t bg, int has_bg)
{
	lv_obj_t *o = th_box (p, x, y, w, h);
	if (has_bg)
	{
		lv_obj_set_style_bg_color (o, lv_color_hex (bg), 0);
		lv_obj_set_style_bg_opa (o, LV_OPA_COVER, 0);
	}
	lv_obj_add_event_cb (o, draw, LV_EVENT_DRAW_MAIN, 0);
	if (touch)
	{
		lv_obj_add_flag (o, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb (o, touch, LV_EVENT_PRESSED, 0);
		lv_obj_add_event_cb (o, touch, LV_EVENT_PRESSING, 0);
		lv_obj_add_event_cb (o, touch, LV_EVENT_RELEASED, 0);
		lv_obj_add_event_cb (o, touch, LV_EVENT_PRESS_LOST, 0);
	}
	return o;
}

static void ext_draw (lv_event_t *e) { lv_event_set_ext_draw_size (e, 6); }

static void panel_info (void)
{
	char b[96];
	int c = s_Chd < SEQED_CHORDS ? s_Chd : 0;
	if (oled ())
	{
		lv_snprintf (b, sizeof b, "STRUM: TAP A CHORD, OR APPLY TO THE %d PAGE%s (1/96 A NOTE)", s_Pages, s_Pages > 1 ? "S" : "");
		set_text (s_PanelInfo[SEQED_STRUM], b);
		lv_snprintf (b, sizeof b, "HUMANISE: TAP A STEP, OR APPLY. TIME %s%d/96  VEL %s%d", "\xC2\xB1", kHumT[s_HumT], "\xC2\xB1", kHumV[s_HumV]);
		set_text (s_PanelInfo[SEQED_HUMAN], b);
		return;
	}
	lv_snprintf (b, sizeof b, "CHORD STAMP   tap a step: %s%s%s on the row's note, %s", s_V.chord[c][0] ? s_V.chord[c] : "-",
		     s_Inv ? " inv " : "", s_Inv ? kInvMap[s_Inv] : "", s_ChStr ? "broken" : "struck together");
	set_text (s_PanelInfo[SEQED_CHORD], b);
	lv_snprintf (b, sizeof b, "STRUM   tap a chord, or APPLY to the %d visible page%s   amount 1/96 step a note",
		     s_Pages, s_Pages > 1 ? "s" : "");
	set_text (s_PanelInfo[SEQED_STRUM], b);
	lv_snprintf (b, sizeof b, "HUMANISE   tap a step, or APPLY to the view   TIME +-%d/96   VEL +-%d", kHumT[s_HumT], kHumV[s_HumV]);
	set_text (s_PanelInfo[SEQED_HUMAN], b);
	lv_snprintf (b, sizeof b, "KEY   %s %s   QNT %s   seed %s", kName[s_Root], kScaleName[s_Scale < 14 ? s_Scale : 0],
		     kQntMap[s_V.qnt >= 0 && s_V.qnt < 3 ? s_V.qnt : 0], s_V.seed_fixed ? "fixed" : "FREE");
	if (s_V.seed_fixed) lv_snprintf (b + strlen (b), sizeof b - strlen (b), " %d", s_V.seed);
	set_text (s_PanelInfo[SEQED_KEY], b);
}

// ---- seqtop: KEY (the pattern's key, scale, QNT, seed), PLAY (the shown track's speed, direction, random notes), STEP
// (the selected note's step: ratchet / arp, FX column 1, random notes, its locks' condition and slide). Every key is one
// console line (core/seq/xcmd.cpp); the view brings the state back.
static void key_send (void)
{
	char l[48];
	lv_snprintf (l, sizeof l, "seq=key:%s:%s:%s", kName[s_Root], kScaleWord[s_Scale < 14 ? s_Scale : 0],
		     kQntWord[s_V.qnt >= 0 && s_V.qnt < 3 ? s_V.qnt : 0]);
	s_V.key = s_Root, s_V.scale = s_Scale, s_KeyV = s_Root, s_ScaleV = s_Scale;	// the guess, until the view says
	command (l);
}
static void x_view (void)
{
	if (s_QntM) th_seg_set (s_QntM, s_V.qnt >= 0 && s_V.qnt < 3 ? s_V.qnt : 0);
	if (s_Free) th_key_text (s_Free, s_V.seed_fixed ? "FIX" : "FREE");
	int sp = -1;
	for (int i = 0; i < 10; i++) if (kSpdOrder[i] == s_V.speed) sp = i;
	if (s_SpdM) th_seg_set (s_SpdM, sp);
	if (s_DirM2) th_seg_set (s_DirM2, s_V.dir >= 0 && s_V.dir < 6 ? s_V.dir : 0);
	if (s_RndM) th_seg_set (s_RndM, s_V.rnd_mode >= 0 && s_V.rnd_mode < 3 ? s_V.rnd_mode : 0);
	if (s_OctM) th_seg_set (s_OctM, s_V.rnd_oct >= 1 && s_V.rnd_oct <= 4 ? s_V.rnd_oct - 1 : 1);
	if (s_WghtM) th_seg_set (s_WghtM, s_V.rnd_wght >= 0 && s_V.rnd_wght < 6 ? s_V.rnd_wght : 0);
	char b[112];
	if (s_V.track < 0) lv_snprintf (b, sizeof b, oled () ? "PLAY: NO SEQ TRACK" : "PLAY   no seq track shown");
	else lv_snprintf (b, sizeof b, oled () ? "PLAY T%d SPD %s %s RND %s" : "PLAY   T%d: speed %s, %s, random notes %s (%d oct, %s)",
			  s_V.track + 1, s_V.speed > 0 && s_V.speed < 10 ? kSpdWord[s_V.speed] : "x1",
			  kDir2Map[s_V.dir >= 0 && s_V.dir < 6 ? s_V.dir : 0], kRndMap[s_V.rnd_mode >= 0 && s_V.rnd_mode < 3 ? s_V.rnd_mode : 0],
			  s_V.rnd_oct, kWghtMap[s_V.rnd_wght >= 0 && s_V.rnd_wght < 6 ? s_V.rnd_wght : 0]);
	set_text (s_PanelInfo[SEQED_PLAY], b);
	int st = s_SelStep >= 0 && s_SelStep < SEQED_STEPS ? s_SelStep : -1;
	int rt = -1, fx = 0, cd = 0;
	if (st >= 0)
	{
		int c = s_V.rt_cnt[st], m = s_V.rt_mode[st];
		rt = !(s_V.xstep[st] & 2) ? 0 : m == 2 ? 5 : m == 4 ? 6 : m == 5 ? 7 : c == 2 ? 1 : c == 3 ? 2 : c == 4 ? 3 : c == 8 ? 4 : -1;
		fx = -1;
		for (int i = 0; i < 6 && fx < 0; i++)
		{
			static const int kc[6] = {0, 7, 7, 5, 9, 12}, kv[6] = {0, 1, -1, 2, 1, 1};
			if (s_V.fx_cmd[st] == kc[i] && (!i || s_V.fx_val[st] == kv[i])) fx = i;
		}
		cd = -1;
		for (int i = 0; i < 4; i++) if (s_V.lcond[st] == kCondCode[i]) cd = i;
	}
	if (s_RtM) th_seg_set (s_RtM, rt);
	if (s_FxM) th_seg_set (s_FxM, fx);
	if (s_CondM) th_seg_set (s_CondM, cd);
	if (s_RndS) th_key_text (s_RndS, st >= 0 && (s_V.xstep[st] & 1) ? "RND ON" : "RND");
	if (s_SlideK) th_key_text (s_SlideK, st >= 0 && (s_V.xstep[st] & 16) ? "SLIDE ON" : "SLIDE");
	if (st < 0) lv_snprintf (b, sizeof b, oled () ? "STEP: SELECT A NOTE (DRAW)" : "STEP   select a note with DRAW: its step's ratchet, FX, random notes, locks");
	else lv_snprintf (b, sizeof b, oled () ? "STEP %d: %s%s%s%s" : "STEP   %d:%s%s%s%s", st + 1, (s_V.xstep[st] & 2) ? " ratchet/arp" : "",
			  (s_V.xstep[st] & 4) ? " FX" : "", (s_V.xstep[st] & 1) ? " random notes" : "", (s_V.xstep[st] & 24) ? " lock cond/slide" : "");
	set_text (s_PanelInfo[SEQED_STEP], b);
}
static void x_send (const char *fmt, const char *a)
{
	if (s_V.track < 0) return;
	char l[80];
	lv_snprintf (l, sizeof l, fmt, s_V.track + 1, a);
	command (l);
	x_view ();
}
static int step_ok (void)
{
	if (s_V.track >= 0 && s_SelStep >= 0 && s_SelStep < SEQED_STEPS) return 1;
	toast ("STEP: SELECT A NOTE WITH DRAW");
	return 0;
}
static void step_send (const char *what, const char *a)
{
	char l[80];
	lv_snprintf (l, sizeof l, "seq=%s:%d:%d:%s", what, s_V.track + 1, s_SelStep + 1, a);
	command (l);
	x_view ();
}
static void seed_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int d = (int) (intptr_t) ud;
	char l[32];
	if (d == 0) lv_snprintf (l, sizeof l, s_V.seed_fixed ? "seq=seed:free" : "seq=seed:%d", s_V.seed);	// FREE <-> fixed
	else lv_snprintf (l, sizeof l, "seq=seed:%d", clampi (s_V.seed + d, 0, 9999));
	if (d == 0) s_V.seed_fixed = !s_V.seed_fixed; else s_V.seed = clampi (s_V.seed + d, 0, 9999), s_V.seed_fixed = 1;
	command (l);
	x_view ();
	panel_info ();
}
static void print_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	if (s_V.track < 0) return;
	char l[24];
	lv_snprintf (l, sizeof l, "seq=print:%d", s_V.track + 1);
	command (l);
	toast ("RANDOM NOTES PRINTED");
}
static void stepkey_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	if (!step_ok ()) return;
	int st = s_SelStep;
	if (ud) step_send ("slide", (s_V.xstep[st] & 16) ? "all:0" : "all:1");
	else step_send ("rndstep", (s_V.xstep[st] & 1) ? "0" : "1");
}

// SWING: the shown track's own (the host's sequencer, not the Rytm's): every second step late by (swing / 50 - 1) of a
// step; the presets checked when one matches, the value in the info line and the header
static void swing_view (void)
{
	char b[96];
	int sw = swing_now (), sel = -1;
	for (int i = 0; i < 8; i++) if (kSwing[i] == sw) sel = i;
	if (s_SwingM) th_seg_set (s_SwingM, sel);
	if (s_V.track < 0) lv_snprintf (b, sizeof b, oled () ? "SWING: NO SEQ TRACK" : "SWING   no seq track shown");
	else if (oled ()) lv_snprintf (b, sizeof b, "SWING T%d: %d %%%s", s_V.track + 1, sw, sw > 50 ? "  EVERY 2ND STEP LATE" : "  STRAIGHT");
	else lv_snprintf (b, sizeof b, "SWING   T%d: %d %%   %s", s_V.track + 1, sw,
			  sw > 50 ? "every second step late (the host's own swing, this track)" : "straight (the host's own swing, this track)");
	set_text (s_PanelInfo[SEQED_SWING], b);
}

static void swing_send (int v)
{
	if (s_V.track < 0) return;
	v = clampi (v, 50, 80);
	if (v > 50) s_SwingLast = v;
	char l[32];
	lv_snprintf (l, sizeof l, "seq=swing:%d:%d", s_V.track + 1, v);
	s_V.swing = v;					// the guess, until the view says
	command (l);
	swing_view ();
	header_update ();
}

static void swing_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int d = (int) (intptr_t) ud;
	if (d == 0) swing_send (swing_now () > 50 ? 50 : s_SwingLast);	// (straight <-> the last swing)
	else swing_send (swing_now () + d);
}

// POLY (seqx E0): the shown track's poly config - VOICES, ALLOCATION, UNISON keys checked as it plays, the info line
// what it resolves to on the track's machine (the instances, the notes at once, the cost)
static void poly_view (void)
{
	char b[96];
	int sv = -1, su = -1;
	for (int i = 0; i < 7; i++) if (kVoicesV[i] == s_V.poly_voices) sv = i;
	for (int i = 0; i < 4; i++) if (kUniV[i] == (s_V.poly_unison > 1 ? s_V.poly_unison : 1)) su = i;
	if (s_VoicesM) th_seg_set (s_VoicesM, sv);
	if (s_AllocM) th_seg_set (s_AllocM, s_V.poly_alloc >= 0 && s_V.poly_alloc < 3 ? s_V.poly_alloc : 0);
	if (s_UniM) th_seg_set (s_UniM, su);
	if (s_V.track < 0) lv_snprintf (b, sizeof b, oled () ? "POLY: NO SEQ TRACK" : "POLY   no seq track shown");
	else if (oled ()) lv_snprintf (b, sizeof b, "POLY T%d %s: %s", s_V.track + 1, kAllocWord[s_V.poly_alloc >= 0 && s_V.poly_alloc < 3 ? s_V.poly_alloc : 0],
				      s_V.poly_info[0] ? s_V.poly_info : "-");
	else lv_snprintf (b, sizeof b, "POLY   T%d: %s", s_V.track + 1, s_V.poly_info[0] ? s_V.poly_info : "-");
	set_text (s_PanelInfo[SEQED_POLY], b);
}

static void poly_send (const char *kv)
{
	if (s_V.track < 0) return;
	char l[48];
	lv_snprintf (l, sizeof l, "seq=poly:%d:%s", s_V.track + 1, kv);
	command (l);
	poly_view ();
}

static void chord_map (void)
{
	int k = 0;
	for (int i = 0; i < 10; i++)
	{
		int c = kChordSet[s_bMore][i];
		if (!c) break;
		s_ChordMap[k++] = s_V.chord[c][0] ? s_V.chord[c] : "?";
	}
	s_ChordMap[k++] = s_bMore ? "<" : "...";
	s_ChordMap[k] = "";
	th_seg_map (s_ChordM, s_ChordMap);
	for (int i = 0; i < k; i++) th_seg_checkable (s_ChordM, i, i < k - 1);
	int sel = -1;
	for (int i = 0; i < k - 1; i++)
		if (kChordSet[s_bMore][i] == s_Chd) sel = i;
	th_seg_set (s_ChordM, sel);
}

static void tool_show (void)
{
	for (int t = SEQED_CHORD; t < SEQED_TOOLS; t++)
		if (t == s_Tool) lv_obj_remove_flag (s_Panel[t], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Panel[t], LV_OBJ_FLAG_HIDDEN);
	if (s_Tool == SEQED_DRAW) lv_obj_remove_flag (s_Lane, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Lane, LV_OBJ_FLAG_HIDDEN);
	panel_info ();
	swing_view ();
	poly_view ();
	x_view ();
	card_update ();
}

static void ctl_cb (lv_obj_t *m, int i, void *ud)
{
	(void) ud;
	if (i < 0) return;
	if (m == s_Tools)
	{
		if (i == SEQED_DRAW && s_bLocked) { lock_set (0); toast ("DRAW: EDITS ON"); }	// T5: editing is this tap
		else if (s_bLocked) lock_set (0);
		s_Tool = i;
		tool_show ();
		return;
	}
	if (m == s_ChordM)
	{
		int k = 0;
		while (k < 10 && kChordSet[s_bMore][k]) k++;
		if (i == k) { s_bMore = !s_bMore; chord_map (); return; }
		s_Chd = kChordSet[s_bMore][i];
	}
	else if (m == s_InvM) s_Inv = i;
	else if (m == s_ChStrM) s_ChStr = i;
	else if (m == s_StrumM) s_StrumI = i;
	else if (m == s_DirM) s_StrumDir = i;
	else if (m == s_HumTM) s_HumT = i;
	else if (m == s_HumVM) s_HumV = i;
	else if (m == s_SwingM) { swing_send (kSwing[i & 7]); return; }
	else if (m == s_VoicesM || m == s_AllocM || m == s_UniM)	// POLY (seqx E0): the guess until the view says
	{
		char kv[24];
		if (m == s_VoicesM) s_V.poly_voices = kVoicesV[i % 7], lv_snprintf (kv, sizeof kv, "voices=%d", s_V.poly_voices);
		else if (m == s_AllocM) s_V.poly_alloc = i % 3, lv_snprintf (kv, sizeof kv, "alloc=%s", kAllocName[i % 3]);
		else s_V.poly_unison = kUniV[i & 3], lv_snprintf (kv, sizeof kv, "unison=%d", s_V.poly_unison);
		poly_send (kv);
		return;
	}
	else if (m == s_RootM || m == s_ScaleM)		// seqtop: the pattern's key and scale (seq=key)
	{
		if (m == s_RootM) s_Root = i; else s_Scale = i;
		key_send ();
		lv_obj_invalidate (s_Grid);
		lv_obj_invalidate (s_Keys);
	}
	else if (m == s_QntM) { s_V.qnt = i % 3; key_send (); }
	else if (m == s_SpdM) { s_V.speed = kSpdOrder[i % 10]; x_send ("seq=spd:%d:%s", kSpdWord[s_V.speed]); return; }
	else if (m == s_DirM2) { s_V.dir = i % 6; x_send ("seq=dir:%d:%s", kDir2Word[s_V.dir]); return; }
	else if (m == s_RndM || m == s_OctM || m == s_WghtM)
	{
		char kv[48];
		if (m == s_RndM) s_V.rnd_mode = i % 3; else if (m == s_OctM) s_V.rnd_oct = i % 4 + 1; else s_V.rnd_wght = i % 6;
		lv_snprintf (kv, sizeof kv, "mode=%s,oct=%d,wght=%s", kRndWord[s_V.rnd_mode % 3], s_V.rnd_oct, kWghtWord[s_V.rnd_wght % 6]);
		x_send ("seq=rnd:%d:%s", kv);
		return;
	}
	else if (m == s_RtM || m == s_FxM || m == s_CondM)
	{
		if (!step_ok ()) { x_view (); return; }
		if (m == s_RtM) step_send ("rtrg", kRtLine[i & 7]);
		else if (m == s_FxM) { char a[16]; lv_snprintf (a, sizeof a, "1:%s", kFxLine[i % 6]); step_send ("fx", a); }
		else { char a[16]; lv_snprintf (a, sizeof a, "all:%s", kCondLine[i & 3]); step_send ("lcond", a); }
		return;
	}
	panel_info ();
}

static void apply_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	step_tool (s_First * 16, (s_First + s_Pages) * 16 - 1);
}

static const char *snap_text (void)
{
	if (oled ()) return s_Snap96 == 96 ? "SNAP 1/16" : s_Snap96 == 48 ? "SNAP 1/32" : s_Snap96 == 24 ? "SNAP 1/64" : "SNAP FREE";	// (T4: named)
	return s_Snap96 == 96 ? "SNAP 1/16" : s_Snap96 == 48 ? "SNAP 1/32" : s_Snap96 == 24 ? "SNAP 1/64" : "SNAP FREE";
}

static void snap_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	s_Snap96 = s_Snap96 == 96 ? 48 : s_Snap96 == 48 ? 24 : s_Snap96 == 24 ? 1 : 96;
	th_key_text (s_Snap, snap_text ());
}

static void undo_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	char l[32];
	if (s_V.track < 0) return;
	lv_snprintf (l, sizeof l, "seqed=%s:%d", ud ? "redo" : "undo", s_V.track);
	command (l);
}

static void delete_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	int k = s_SelStep >= 0 ? find_note (s_SelStep, s_SelPitch) : -1;
	if (k < 0) return;
	if (s_bLocked) { toast ("VIEW: TAP DRAW TO EDIT"); return; }	// T5
	int st = s_SelStep, pi = s_SelPitch;
	local_del (k);
	select_note (-1, -1);
	cmd_note ("del", st, pi, 0, 0, 0, 2);
}

static void track_cb (lv_obj_t *m, int i, void *ud)
{
	(void) m; (void) ud;
	if (i < 0 || i >= SEQED_TRACKS || s_V.tracks[i] < 0) return;
	char l[32];
	lv_snprintf (l, sizeof l, "seqed=show:%d", s_V.tracks[i]);
	command (l);
}

static void rytm_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i; (void) ud;
	if (s_RytmCb) s_RytmCb ();
}

// OLED: the pitch column's keys right of the grid (up, the window's pages, down)
static void pitch_cb (lv_obj_t *o, int i, void *ud)
{
	(void) o; (void) i;
	int d = (int) (intptr_t) ud;
	s_TouchMs = lv_tick_get ();
	if (d == 0)						// pages: 1, 2, 4, 8 (as far as the pattern has them)
	{
		int np = s_Pages >= npages () ? 1 : s_Pages * 2;
		window (s_First, np, s_Top);
		return;
	}
	window (s_First, s_Pages, s_Top + d * 6);
}

static char s_TrackTxt[SEQED_TRACKS][8];
static const char *s_TrackMap[SEQED_TRACKS + 1];

static void header_update (void)
{
	char b[96], pat[8] = "-";
	if (s_V.pattern >= 0) lv_snprintf (pat, sizeof pat, "%c%02d", 'A' + s_V.pattern / 16, s_V.pattern % 16 + 1);
	const char *tr = s_V.transport == 1 ? LV_SYMBOL_PLAY : s_V.transport == 2 ? LV_SYMBOL_PAUSE : LV_SYMBOL_STOP;
	char pg[16];
	if (s_Pages > 1) lv_snprintf (pg, sizeof pg, "PG %d-%d/%d", s_First + 1, s_First + s_Pages, npages ());
	else lv_snprintf (pg, sizeof pg, "PG %d/%d", s_First + 1, npages ());
	if (oled ())
	{
		char l[48], r[64];
		if (s_V.track >= 0) lv_snprintf (l, sizeof l, "SEQ %s T%d", pat, s_V.track + 1);
		else lv_snprintf (l, sizeof l, "SEQ %s", pat);
		char sw[12] = "";
		if (s_V.track >= 0 && swing_now () > 50) lv_snprintf (sw, sizeof sw, "  SW%d", swing_now ());
		if (s_V.bpm10 > 0) lv_snprintf (r, sizeof r, "%s  %s %d.%d%s", s_V.project[0] ? s_V.project : "-", tr, s_V.bpm10 / 10, s_V.bpm10 % 10, sw);
		else lv_snprintf (r, sizeof r, "%s  %s%s", s_V.project[0] ? s_V.project : "-", tr, sw);
		th_titlebar_set (s_TitleBar, l, r, 0);
		lv_snprintf (b, sizeof b, "%d", s_Pages);
		th_key_text (s_Zoom, b);
		th_key_sub (s_Zoom, "PG");
	}
	else
	{
		char sw[20] = "";					// (the strip shows the pages: the line keeps its width)
		if (s_V.track >= 0 && swing_now () > 50) lv_snprintf (sw, sizeof sw, "  SW %d%%", swing_now ());
		if (s_Pages > 1) lv_snprintf (pg, sizeof pg, "%d-%d/%d", s_First + 1, s_First + s_Pages, npages ());
		else lv_snprintf (pg, sizeof pg, "%d/%d", s_First + 1, npages ());
		if (s_V.bpm10 > 0) lv_snprintf (b, sizeof b, "%s %d.%d  %s%s", tr, s_V.bpm10 / 10, s_V.bpm10 % 10, pg, sw);
		else lv_snprintf (b, sizeof b, "%s  %s%s", tr, pg, sw);
		set_text (s_Title, b);
		if (s_V.track >= 0)
			lv_snprintf (b, sizeof b, "%s  %s  T%d", s_V.project[0] ? s_V.project : "-", pat, s_V.track + 1);
		else lv_snprintf (b, sizeof b, "%s  %s", s_V.project[0] ? s_V.project : "-", pat);
		set_text (s_Where, b);
	}
	int changed = 0, sel = -1;
	for (int i = 0; i < SEQED_TRACKS; i++)
	{
		char t[8];
		if (s_V.tracks[i] >= 0) lv_snprintf (t, sizeof t, "T%d", s_V.tracks[i] + 1); else lv_snprintf (t, sizeof t, "-");
		if (strcmp (t, s_TrackTxt[i])) { strcpy (s_TrackTxt[i], t); changed = 1; }
		if (s_V.tracks[i] == s_V.track && s_V.track >= 0) sel = i;
	}
	if (changed || !s_TrackMap[0] || th_seg_count (s_Tracks) != SEQED_TRACKS)
	{
		for (int i = 0; i < SEQED_TRACKS; i++) s_TrackMap[i] = s_TrackTxt[i];
		s_TrackMap[SEQED_TRACKS] = "";
		th_seg_map (s_Tracks, s_TrackMap);
	}
	for (int i = 0; i < SEQED_TRACKS; i++) th_seg_enable (s_Tracks, i, s_V.tracks[i] >= 0);
	if (sel >= 0) th_seg_set (s_Tracks, sel);
	th_key_enable (s_Undo, s_V.undo != 0);
	th_key_enable (s_Redo, s_V.redo != 0);
	const char *empty = !s_V.valid ? "No Sidecar sequencer in this build" :
			    s_V.track < 0 ? (oled () ? "NO SEQ TRACK. MARK A RYTM TRACK AS A\nSIDECAR SEQ TRACK: SEQ=TRACK:T:ON,\nOR THE RYTM'S SEQ PAGE"
					    : "No seq track. Mark a Rytm track as a Sidecar seq track:\nconsole seq=track:T:on, "
					    "or the Rytm's SEQ page") : "";
	set_text (s_Empty, empty);
	if (empty[0]) lv_obj_remove_flag (s_Empty, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Empty, LV_OBJ_FLAG_HIDDEN);
}

// the pitch window follows notes that would be out of sight: notes new since the last view (a chord written on the
// Rytm, recorded, pasted) when none of them shows, and a track or pattern opened with none of its notes in the window.
// The rows centre on the notes (the highest kept in view); a window that already shows one of them stays put
static int seen (const sidecar_seq_view_t *v, int step, int pitch)
{
	for (int i = 0; i < v->n; i++)
		if (v->note[i].step == step && v->note[i].pitch == pitch) return 1;
	return 0;
}
static void follow_notes (const sidecar_seq_view_t *v, int opened)
{
	int lo = 128, hi = -1, shown = 0;
	const int bot = s_Top - rows () + 1;
	for (int i = 0; i < v->n; i++)
	{
		const sidecar_seq_note_t *n = &v->note[i];
		if (!opened && s_bHave && seen (&s_V, n->step, n->pitch)) continue;
		if (n->pitch >= bot && n->pitch <= s_Top) shown = 1;
		if (n->pitch < lo) lo = n->pitch;
		if (n->pitch > hi) hi = n->pitch;
	}
	if (hi < 0 || shown) return;
	int top = (lo + hi) / 2 + rows () / 2;
	if (top < hi + 1) top = hi + 1;
	window (s_First, s_Pages, top);
}

void sidecar_seqed_view (const sidecar_seq_view_t *v)
{
	if (g.mode != G_NONE)				// a gesture in progress: the view waits for its end
	{
		memcpy (&s_Pend, v, sizeof *v);
		s_bPend = 1;
		return;
	}
	int news = !s_bHave || v->gen != s_V.gen || v->track != s_V.track || v->pattern != s_V.pattern || v->npages != s_V.npages
		   || v->length != s_V.length;
	int chords = !s_bHave || memcmp (v->chord, s_V.chord, sizeof v->chord);
	int opened = !s_bHave || v->track != s_V.track || v->pattern != s_V.pattern;
	if (opened || v->gen != s_V.gen) follow_notes (v, opened);
	int was_playing = s_V.transport == 1;
	memcpy (&s_V, v, sizeof *v);
	if (!playing () && s_bLocked) lock_set (0);			// T5: stopped, the grid edits again
	else if (playing () && !was_playing) edited ();			// started: VIEW after the idle time from now
	s_bHave = 1;
	if (chords) chord_map ();
	if (v->key != s_KeyV || v->scale != s_ScaleV)		// seqtop: the pattern's key moved (here or elsewhere): KEY shows it
	{
		s_KeyV = v->key, s_ScaleV = v->scale;
		if (v->key >= 0 && v->key < 12 && v->scale >= 0 && v->scale < 14)
		{
			s_Root = v->key, s_Scale = v->scale;
			if (s_RootM) th_seg_set (s_RootM, s_Root);
			if (s_ScaleM) th_seg_set (s_ScaleM, s_Scale < 5 ? s_Scale : -1);
			news = 1;
		}
	}
	header_update ();
	swing_view ();
	poly_view ();
	x_view ();
	panel_info ();
	if (news)
	{
		if (s_First + s_Pages > npages ()) window (0, s_Pages, s_Top);
		inval_all ();
		card_update ();
	}
	// U1: NOTE EDIT on the Rytm - its highlighted note selected here (the card shows its values), the window paged and
	// scrolled to it; the playhead does not move the window meanwhile (sidecar_seqed_playhead)
	if (v->ne_on && v->ne_gen != s_NeGen && g.mode == G_NONE)	// (a touch in progress: the next view after it)
	{
		s_NeGen = v->ne_gen;
		inval_all ();
		if (v->ne_step >= 0 && v->ne_pitch >= 0)
		{
			int pg = v->ne_step / 16, top = s_Top, first = s_First;
			if (pg < s_First || pg >= s_First + s_Pages) first = pg / s_Pages * s_Pages;
			// off the window, or in its top rows (the card goes above the note, under the window's own pointers there):
			// the note's row to the middle
			if (v->ne_pitch > s_Top - 4 || v->ne_pitch < s_Top - rows () + 2) top = v->ne_pitch + rows () / 2;
			window (first, s_Pages, top);
			select_note (v->ne_step, v->ne_pitch);
		}
	}
	else if (!v->ne_on && s_NeGen != ~0u)
	{
		s_NeGen = ~0u;
		inval_all ();
	}
	// rui1051: the Rytm's PAGE moved its edit page (stopped; playing, the window follows the play head): the window holds it
	if (v->edit_page != s_EditPg && g.mode == G_NONE && !v->ne_on)	// (a touch in progress: at the next view after it)
	{
		s_EditPg = v->edit_page;
		if (s_EditPg >= 0 && s_EditPg < npages () && !playing () && (s_EditPg < s_First || s_EditPg >= s_First + s_Pages))
			window (s_EditPg / s_Pages * s_Pages, s_Pages, s_Top);
	}
}

// the notes that start or stop playing between two play positions (1/96 step): their colour changes
static void inval_lit (int a, int b)
{
	if (a == b) return;
	for (int i = 0; i < s_V.n; i++)
	{
		const sidecar_seq_note_t *n = &s_V.note[i];
		int on = n->step * STEP96 + n->micro, off = on + n->len;
		int la = a >= 0 && a >= on && a < off, lb = b >= 0 && b >= on && b < off;
		if (la != lb) inval_note (n->step, n->micro, n->pitch, n->len);
	}
}

void sidecar_seqed_playhead (int pos)
{
	if (!s_bLocked && playing () && g.mode == G_NONE && lv_tick_elaps (s_EditMs) > kViewIdleMs) lock_set (1);	// T5
	int old = s_Play;
	int a = play96 ();
	s_Play = pos;
	inval_lit (a, play96 ());
	if (old / 16000 != pos / 16000 || (old < 0) != (pos < 0)) lv_obj_invalidate (s_Strip);
	if (pos < 0) { lv_obj_add_flag (s_Head, LV_OBJ_FLAG_HIDDEN); return; }
	int p96 = (int) ((long) pos * STEP96 / 1000);
	int x = x_of (p96);
	static int s_bIn;					// window () calls back here
	if ((x < 0 || x >= GRID_W) && g.mode == G_NONE && lv_tick_elaps (s_TouchMs) > 3000 && s_Pages < npages () && !s_bIn
	    && !s_V.ne_on)					// (U1: NOTE EDIT's note holds the window)
	{
		s_bIn = 1;
		window (p96 / PAGE96 / s_Pages * s_Pages, s_Pages, s_Top);	// follow: the window that holds the play head
		x = x_of (p96);
		s_bIn = 0;
	}
	if (x < 0 || x >= GRID_W) { lv_obj_add_flag (s_Head, LV_OBJ_FLAG_HIDDEN); return; }
	lv_obj_set_x (s_Head, GRID_X + x);
	lv_obj_remove_flag (s_Head, LV_OBJ_FLAG_HIDDEN);
}

void sidecar_seqed_touch2 (int down, int x, int y)
{
	if (!down)
	{
		if (g.mode == G_PINCH) g.mode = G_EMPTY, g.moved = 1;	// the first finger's release does nothing more
		g.two = 0;
		return;
	}
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	x -= c.x1; y -= c.y1;
	if (!g.two)
	{
		if (g.mode != G_EMPTY && g.mode != G_PAN && g.mode != G_NOTE) return;
		g.two = 1;
		if (g.mode == G_NOTE && g.moved) inval_note (g.nstep, g.nmicro, g.npitch, g.nlen);
		g.mode = G_PINCH;
		g.d0x = iabs (x - g.x); g.d0y = iabs (y - g.y);
		g.pages0 = s_Pages; g.rowh0 = s_RowH; g.first0 = s_First;
		return;
	}
	if (g.mode != G_PINCH) return;
	int dx = iabs (x - g.x), dy = iabs (y - g.y), min = oled () ? 20 : 40;
	int pages = g.pages0, rh = g.rowh0;
	if (g.d0x > min && dx > 0) pages = clampi ((g.pages0 * g.d0x + dx / 2) / dx, 1, 8);	// apart: fewer pages
	if (g.d0y > min) rh = clampi (g.rowh0 * dy / g.d0y, G->row_min, G->row_max);
	if (rh != s_RowH) { s_RowH = rh; inval_all (); }
	window (g.first0, pages, s_Top);
}

void sidecar_seqed_set_rytm_cb (void (*fn) (void)) { s_RytmCb = fn; }

void sidecar_seqed_show (int on)
{
	int was = !lv_obj_has_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
	if (on) lv_obj_remove_flag (s_Page, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag (s_Page, LV_OBJ_FLAG_HIDDEN);
	if (on && !was) lock_set (playing ());		// T5: opened while the Rytm plays: VIEW
}

int sidecar_seqed_locked (void) { return s_bLocked; }
void sidecar_seqed_tool_xy (int tool, int *x, int *y)
{
	lv_area_t c;
	lv_obj_get_coords (th_seg_item (s_Tools, tool), &c);
	*x = (c.x1 + c.x2) / 2, *y = (c.y1 + c.y2) / 2;
}
const char *sidecar_seqed_toast (void) { return s_Toast && !lv_obj_has_flag (s_Toast, LV_OBJ_FLAG_HIDDEN) ? lv_label_get_text (s_Toast) : ""; }

void sidecar_seqed_geom (int *gx, int *gy, int *gw, int *gh, int *rowh, int *lane_y, int *lane_h)
{
	lv_area_t c;
	lv_obj_get_coords (s_Grid, &c);
	*gx = c.x1; *gy = c.y1; *gw = GRID_W; *gh = GRID_H; *rowh = s_RowH;
	lv_obj_get_coords (s_Lane, &c);
	*lane_y = c.y1; *lane_h = LANE_H;
}

void sidecar_seqed_create (void *parent, int y0, sidecar_seqed_command_t cmd, void *param)
{
	s_Cmd = cmd;
	s_CmdParam = param;
	geo_size (oled () ? &kGeoOled : &kGeoClassic);
	if (!s_bInit)					// the model and the editor's state outlive a theme switch
	{
		s_bInit = 1;
		s_V.track = -1;
		s_V.pattern = -1;
		for (int i = 0; i < SEQED_TRACKS; i++) s_V.tracks[i] = -1;
		for (int i = 0; i < SEQED_CHORDS; i++) s_V.chord[i][0] = 0;
		s_RowH = G->row_h;
	}
	else
	{
		memset (&g, 0, sizeof g);		// a gesture cut off by the rebuild (its object deleted): no release comes
		s_RowH = clampi (s_RowH * G->row_h / (oled () ? kGeoClassic.row_h : kGeoOled.row_h), G->row_min, G->row_max);
	}
	memset (s_TrackTxt, 0, sizeof s_TrackTxt);
	s_TrackMap[0] = 0;
	const int PW = oled () ? TH->w - 48 : TH->w, dx = TH->dx;	// the page's width; what 1024x600 adds
	lv_obj_t *pg = th_box ((lv_obj_t *) parent, 0, y0, PW, G->pg_h);
	s_Page = pg;
	const lv_font_t *mic = TH->f_micro;

	// header
	if (oled ())
	{
		s_TitleBar = th_titlebar (pg, 0, 0, PW, 13);
		s_Title = s_Where = 0;
		s_Tracks = th_seg (pg, G->tracks_x, G->tracks_y, G->tracks_w, G->tracks_h, (const char *const[]) {"-", "-", "-", "-", ""},
				   TH_KEY, track_cb, 0);
		s_Snap = button (pg, G->snap_x, G->snap_y, G->snap_w, G->snap_h, snap_text (), TH_KEY, snap_cb, 0);
	}
	else
	{
		s_TitleBar = 0;
		lv_obj_t *acc = th_box (pg, 12, 8, 3, 40);
		lv_obj_set_style_bg_color (acc, lv_color_hex (TH->accent), 0);
		lv_obj_set_style_bg_opa (acc, LV_OPA_COVER, 0);
		lv_obj_t *t = label (pg, 22, 7, &lv_font_montserrat_12, 0x1E7A3E, "SIDECAR SEQ");
		lv_obj_set_style_text_letter_space (t, 2, 0);
		s_Title = label (pg, 132, 7, &lv_font_montserrat_12, TH->ink, "");
		s_Where = label (pg, 22, 25, &lv_font_montserrat_18, TH->ink, "-");
		lv_obj_set_width (s_Where, 226 + dx / 4);
		lv_label_set_long_mode (s_Where, LV_LABEL_LONG_MODE_DOTS);
		s_Tracks = th_seg (pg, G->tracks_x, G->tracks_y, G->tracks_w, G->tracks_h, (const char *const[]) {"-", "-", "-", "-", ""},
				   TH_KEY, track_cb, 0);
		th_seg_font (s_Tracks, &lv_font_montserrat_14);
		button (pg, 660 + dx, 4, 132, 48, "RYTM " LV_SYMBOL_RIGHT, TH_KEY_FUNC, rytm_cb, &lv_font_montserrat_16);
		s_Snap = button (pg, G->snap_x, G->snap_y, G->snap_w, G->snap_h, snap_text (), TH_KEY, snap_cb, &lv_font_montserrat_12);
	}
	s_Strip = drawn (pg, G->strip_x, G->strip_y, G->strip_w, G->strip_h, strip_draw, strip_cb, 0, 0);
	lv_obj_add_event_cb (s_Strip, ext_draw, LV_EVENT_REFR_EXT_DRAW_SIZE, 0);	// CLASSIC: the keys' glow

	// the roll, in a display window (CLASSIC: the hardware's bezel around it)
	if (!oled ()) th_panel (pg, G->keys_x - 6, G->rul_y - 4, G->grid_x + G->grid_w - G->keys_x + 12, G->grid_y + G->grid_h - G->rul_y + 10, "", TH_PANEL_WINDOW);
	s_Ruler = drawn (pg, GRID_X, G->rul_y, GRID_W, G->rul_h, ruler_draw, 0, 0, 0);
	s_Keys = drawn (pg, G->keys_x, GRID_Y, KEYS_W, GRID_H, keys_draw, keys_cb, TH->win, 1);
	s_Grid = drawn (pg, GRID_X, GRID_Y, GRID_W, GRID_H, grid_draw, grid_cb, oled () ? 0 : TH->row_out, 1);
	s_Head = th_box (pg, GRID_X, G->rul_y, G->head_w, GRID_Y + GRID_H - G->rul_y);
	lv_obj_set_style_bg_color (s_Head, lv_color_hex (TH->play), 0);
	lv_obj_set_style_bg_opa (s_Head, oled () ? LV_OPA_COVER : LV_OPA_80, 0);
	lv_obj_add_flag (s_Head, LV_OBJ_FLAG_HIDDEN);
	s_Empty = label (pg, GRID_X + (oled () ? 6 : 40), GRID_Y + (oled () ? 40 : 110), oled () ? mic : &lv_font_montserrat_16, TH->win_dim, "");
	lv_obj_set_width (s_Empty, GRID_W - (oled () ? 12 : 80));
	lv_obj_set_style_text_align (s_Empty, LV_TEXT_ALIGN_CENTER, 0);
	if (oled ())					// pitch up, the window's pages, pitch down
	{
		const int px = GRID_X + GRID_W + 3, pw = PW - px, ph = 36 + TH->dy / 6;	// 318, 34 x 36 at 400x240
		th_key (pg, px, GRID_Y, pw, ph, LV_SYMBOL_UP, TH_KEY, pitch_cb, (void *) (intptr_t) 1);
		s_Zoom = th_key (pg, px, GRID_Y + ph + 4, pw, GRID_H - 2 * ph - 8, "2", TH_KEY, pitch_cb, (void *) (intptr_t) 0);
		th_key (pg, px, GRID_Y + GRID_H - ph, pw, ph, LV_SYMBOL_DOWN, TH_KEY, pitch_cb, (void *) (intptr_t) -1);
	}
	else s_Zoom = 0;

	// the lane, and the tools' controls in its place
	s_Lane = drawn (pg, LANE_X, G->lane_y, G->lane_w, LANE_H, lane_draw, lane_cb, TH->win, !oled ());
	if (!oled ()) lv_obj_set_style_radius (s_Lane, 6, 0);
	for (int t = SEQED_CHORD; t < SEQED_TOOLS; t++)
	{
		lv_obj_t *p = th_box (pg, LANE_X, G->lane_y, G->lane_w, LANE_H);
		if (!oled ())
		{
			lv_obj_set_style_border_color (p, lv_color_hex (0x8A8C90), 0);
			lv_obj_set_style_border_width (p, 1, 0);
			lv_obj_set_style_radius (p, 6, 0);
		}
		lv_obj_add_flag (p, LV_OBJ_FLAG_HIDDEN);
		s_PanelInfo[t] = oled () ? label (p, 3, 1, mic, TH->dim, "") : label (p, 8, 2, &lv_font_montserrat_12, TH->ink, "");
		s_Panel[t] = p;
	}
	static const char *const kNoChords[] = {"-", ""};
	// seqtop: a layout sets only its own (a rebuild in the other style must not keep the old one's deleted objects)
	s_QntM = s_Free = s_SpdM = s_DirM2 = s_RndM = s_OctM = s_WghtM = s_RtM = s_FxM = s_RndS = s_CondM = s_SlideK = 0;
	if (oled ())
	{
		const int K = TH_KEY;				// the body face: chord names read at arm's length
#define LX(v)	((v) * G->lane_w / 352)			// the 400x240 layout's x and widths, stretched to the lane
#define LY(v)	((v) * LANE_H / 44)
		s_ChordM = seg (s_Panel[SEQED_CHORD], 0, 0, G->lane_w, LY (21), kNoChords, -1, K);
		label (s_Panel[SEQED_CHORD], 3, LY (23) + (LY (21) - 7) / 2, mic, TH->dim, "INV");
		s_InvM = seg (s_Panel[SEQED_CHORD], 24, LY (23), LX (150) - 24, LY (21), kInvMap, s_Inv, K);
		label (s_Panel[SEQED_CHORD], LX (158), LY (23) + (LY (21) - 7) / 2, mic, TH->dim, "STRUM");
		s_ChStrM = seg (s_Panel[SEQED_CHORD], LX (158) + 34, LY (23), G->lane_w - LX (158) - 34, LY (21), kChStrMap, s_ChStr, K);
		lv_obj_add_flag (s_PanelInfo[SEQED_CHORD], LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_width (s_PanelInfo[SEQED_STRUM], G->lane_w - 4);
		lv_label_set_long_mode (s_PanelInfo[SEQED_STRUM], LV_LABEL_LONG_MODE_CLIP);
		s_StrumM = seg (s_Panel[SEQED_STRUM], 0, 12, LX (156), LANE_H - 12, kStrumMap, s_StrumI, K);
		s_DirM = seg (s_Panel[SEQED_STRUM], LX (162), 12, LX (255) - LX (162), LANE_H - 12, kDirMap, s_StrumDir, K);
		button (s_Panel[SEQED_STRUM], LX (261), 12, G->lane_w - LX (261), LANE_H - 12, "APPLY", TH_KEY_ACCENT, apply_cb, 0);
		lv_obj_set_width (s_PanelInfo[SEQED_HUMAN], G->lane_w - 4);
		lv_label_set_long_mode (s_PanelInfo[SEQED_HUMAN], LV_LABEL_LONG_MODE_CLIP);
		label (s_Panel[SEQED_HUMAN], 3, 12 + (LANE_H - 12 - 7) / 2 + 1, mic, TH->dim, "T");
		s_HumTM = seg (s_Panel[SEQED_HUMAN], 12, 12, LX (141) - 12, LANE_H - 12, kHumTMap, s_HumT, K);
		label (s_Panel[SEQED_HUMAN], LX (147), 12 + (LANE_H - 12 - 7) / 2 + 1, mic, TH->dim, "V");
		s_HumVM = seg (s_Panel[SEQED_HUMAN], LX (147) + 9, 12, LX (255) - LX (147) - 9, LANE_H - 12, kHumVMap, s_HumV, K);
		button (s_Panel[SEQED_HUMAN], LX (261), 12, G->lane_w - LX (261), LANE_H - 12, "APPLY", TH_KEY_ACCENT, apply_cb, 0);
		s_RootM = seg (s_Panel[SEQED_KEY], 0, 0, G->lane_w, LY (21), kRootMap, s_Root, K);
		th_seg_gap (s_RootM, 2);
		s_ScaleM = seg (s_Panel[SEQED_KEY], 0, LY (23), LX (190), LY (21), kScaleMap, s_Scale, K);
		s_QntM = seg (s_Panel[SEQED_KEY], LX (194), LY (23), LX (292) - LX (194), LY (21), kQntMap, 0, K);	// seqtop
		s_Free = th_key (s_Panel[SEQED_KEY], LX (296), LY (23), G->lane_w - LX (296), LY (21), "FREE", TH_KEY, seed_cb, 0);
		lv_obj_add_flag (s_PanelInfo[SEQED_KEY], LV_OBJ_FLAG_HIDDEN);
		// seqtop PLAY: speed; direction, random notes. STEP: ratchet / arp; FX 1, the locks' condition, random notes
		s_SpdM = seg (s_Panel[SEQED_PLAY], 0, 0, G->lane_w, LY (21), kSpdMap, -1, K);
		s_DirM2 = seg (s_Panel[SEQED_PLAY], 0, LY (23), LX (212), LY (21), kDir2Map, 0, K);
		s_RndM = seg (s_Panel[SEQED_PLAY], LX (216), LY (23), G->lane_w - LX (216), LY (21), kRndMapO, 0, K);
		lv_obj_add_flag (s_PanelInfo[SEQED_PLAY], LV_OBJ_FLAG_HIDDEN);
		s_RtM = seg (s_Panel[SEQED_STEP], 0, 0, G->lane_w, LY (21), kRtMapO, -1, K);
		s_FxM = seg (s_Panel[SEQED_STEP], 0, LY (23), LX (176), LY (21), kFxMap, -1, K);
		s_CondM = seg (s_Panel[SEQED_STEP], LX (180), LY (23), LX (296) - LX (180), LY (21), kCondMap, -1, K);
		s_RndS = th_key (s_Panel[SEQED_STEP], LX (300), LY (23), G->lane_w - LX (300), LY (21), "RND", TH_KEY, stepkey_cb, 0);
		lv_obj_add_flag (s_PanelInfo[SEQED_STEP], LV_OBJ_FLAG_HIDDEN);
		// SWING: the info line, then - / the presets / + (the value's own key: the info line; - and + by 1 %)
		lv_obj_set_width (s_PanelInfo[SEQED_SWING], G->lane_w - 4);
		lv_label_set_long_mode (s_PanelInfo[SEQED_SWING], LV_LABEL_LONG_MODE_CLIP);
		th_key (s_Panel[SEQED_SWING], 0, 12, LX (34), LANE_H - 12, "-", TH_KEY, swing_cb, (void *) (intptr_t) -1);
		s_SwingM = seg (s_Panel[SEQED_SWING], LX (37), 12, LX (278), LANE_H - 12, kSwingMap, -1, K);
		th_key (s_Panel[SEQED_SWING], LX (318), 12, G->lane_w - LX (318), LANE_H - 12, "+", TH_KEY, swing_cb, (void *) (intptr_t) 1);
		// POLY (seqx E0): the info line, then VOICES / ALLOCATION / UNISON
		lv_obj_set_width (s_PanelInfo[SEQED_POLY], G->lane_w - 4);
		lv_label_set_long_mode (s_PanelInfo[SEQED_POLY], LV_LABEL_LONG_MODE_CLIP);
		s_VoicesM = seg (s_Panel[SEQED_POLY], 0, 12, LX (190), LANE_H - 12, kVoicesMap, -1, K);
		s_AllocM = seg (s_Panel[SEQED_POLY], LX (194), 12, LX (290) - LX (194), LANE_H - 12, kAllocMapO, 0, K);
		s_UniM = seg (s_Panel[SEQED_POLY], LX (294), 12, G->lane_w - LX (294), LANE_H - 12, kUniMapO, 0, K);
	}
	else
	{
#define CX(v)	((v) * G->lane_w / 684)			// the 800x480 layout's x and widths, stretched to the lane
		const int ky = 20 + (LANE_H - 70) / 4, kh = 48 + (LANE_H - 70) / 2;	// 20, 48 at 800x480
		s_ChordM = seg (s_Panel[SEQED_CHORD], 4, ky, CX (450), kh, kNoChords, -1, TH_KEY);
		th_seg_font (s_ChordM, &lv_font_montserrat_12);		// eleven keys of 37 px: "SUS4" fits at 12
		s_InvM = seg (s_Panel[SEQED_CHORD], CX (462), ky, CX (112), kh, kInvMap, s_Inv, TH_KEY);
		lv_obj_set_width (s_PanelInfo[SEQED_CHORD], CX (450));
		lv_label_set_long_mode (s_PanelInfo[SEQED_CHORD], LV_LABEL_LONG_MODE_DOTS);
		label (s_Panel[SEQED_CHORD], CX (462) + 2, 2, &lv_font_montserrat_12, TH->ink, "INVERSION");
		label (s_Panel[SEQED_CHORD], CX (582) + 2, 2, &lv_font_montserrat_12, TH->ink, "STRUM /96");
		s_ChStrM = seg (s_Panel[SEQED_CHORD], CX (582), ky, G->lane_w - 4 - CX (582), kh, kChStrMap, s_ChStr, TH_KEY);
		s_StrumM = seg (s_Panel[SEQED_STRUM], 4, ky, CX (330), kh, kStrumMap, s_StrumI, TH_KEY);
		s_DirM = seg (s_Panel[SEQED_STRUM], CX (342), ky, CX (160), kh, kDirMap, s_StrumDir, TH_KEY);
		button (s_Panel[SEQED_STRUM], CX (520), ky, G->lane_w - 4 - CX (520), kh, "APPLY VIEW", TH_KEY_FUNC, apply_cb, &lv_font_montserrat_16);
		label (s_Panel[SEQED_HUMAN], 8, ky + kh / 2 - 8, &lv_font_montserrat_14, TH->ink, "TIME");
		s_HumTM = seg (s_Panel[SEQED_HUMAN], 52, ky, CX (282) - 52, kh, kHumTMap, s_HumT, TH_KEY);
		label (s_Panel[SEQED_HUMAN], CX (296), ky + kh / 2 - 8, &lv_font_montserrat_14, TH->ink, "VEL");
		s_HumVM = seg (s_Panel[SEQED_HUMAN], CX (296) + 36, ky, CX (512) - CX (296) - 36, kh, kHumVMap, s_HumV, TH_KEY);
		button (s_Panel[SEQED_HUMAN], CX (520), ky, G->lane_w - 4 - CX (520), kh, "APPLY VIEW", TH_KEY_FUNC, apply_cb, &lv_font_montserrat_16);
		// seqtop: KEY, PLAY and STEP in two rows under their info line
		const int r1 = 18, rh = (LANE_H - 22) / 2, r2 = r1 + rh + 2;	// 18, 24, 44 at 800x480
		const lv_font_t *f12 = &lv_font_montserrat_12;
		s_RootM = seg (s_Panel[SEQED_KEY], 4, r1, CX (440), rh, kRootMap, s_Root, TH_KEY);
		th_seg_gap (s_RootM, 2);
		th_seg_font (s_RootM, f12);
		s_ScaleM = seg (s_Panel[SEQED_KEY], CX (452), r1, G->lane_w - 4 - CX (452), rh, kScaleMap, s_Scale, TH_KEY);
		th_seg_font (s_ScaleM, f12);
		label (s_Panel[SEQED_KEY], 8, r2 + rh / 2 - 7, f12, TH->ink, "QNT");
		s_QntM = seg (s_Panel[SEQED_KEY], 40, r2, CX (240) - 40, rh, kQntMap, 0, TH_KEY);
		th_seg_font (s_QntM, f12);
		label (s_Panel[SEQED_KEY], CX (252), r2 + rh / 2 - 7, f12, TH->ink, "SEED");
		th_key_font (th_key (s_Panel[SEQED_KEY], CX (300), r2, CX (60), rh, "-", TH_KEY, seed_cb, (void *) (intptr_t) -1), f12);
		s_Free = button (s_Panel[SEQED_KEY], CX (366), r2, CX (110), rh, "FREE", TH_KEY, seed_cb, f12);
		th_key_font (th_key (s_Panel[SEQED_KEY], CX (482), r2, CX (60), rh, "+", TH_KEY, seed_cb, (void *) (intptr_t) 1), f12);
		s_SpdM = seg (s_Panel[SEQED_PLAY], 4, r1, CX (340), rh, kSpdMap, -1, TH_KEY);
		th_seg_font (s_SpdM, f12);
		s_DirM2 = seg (s_Panel[SEQED_PLAY], CX (352), r1, G->lane_w - 4 - CX (352), rh, kDir2Map, 0, TH_KEY);
		th_seg_font (s_DirM2, f12);
		s_RndM = seg (s_Panel[SEQED_PLAY], 4, r2, CX (200), rh, kRndMap, 0, TH_KEY);
		th_seg_font (s_RndM, f12);
		s_OctM = seg (s_Panel[SEQED_PLAY], CX (212), r2, CX (120), rh, kOctMap, 1, TH_KEY);
		th_seg_font (s_OctM, f12);
		s_WghtM = seg (s_Panel[SEQED_PLAY], CX (344), r2, CX (264), rh, kWghtMap, 0, TH_KEY);
		th_seg_font (s_WghtM, f12);
		button (s_Panel[SEQED_PLAY], CX (616), r2, G->lane_w - 4 - CX (616), rh, "PRINT", TH_KEY_FUNC, print_cb, f12);
		s_RtM = seg (s_Panel[SEQED_STEP], 4, r1, CX (380), rh, kRtMap, -1, TH_KEY);
		th_seg_font (s_RtM, f12);
		s_FxM = seg (s_Panel[SEQED_STEP], CX (392), r1, G->lane_w - 4 - CX (392), rh, kFxMap, -1, TH_KEY);
		th_seg_font (s_FxM, f12);
		s_RndS = button (s_Panel[SEQED_STEP], 4, r2, CX (120), rh, "RND", TH_KEY, stepkey_cb, f12);
		label (s_Panel[SEQED_STEP], CX (136), r2 + rh / 2 - 7, f12, TH->ink, "LOCKS");
		s_CondM = seg (s_Panel[SEQED_STEP], CX (192), r2, CX (260), rh, kCondMap, -1, TH_KEY);
		th_seg_font (s_CondM, f12);
		s_SlideK = th_key (s_Panel[SEQED_STEP], CX (464), r2, CX (130), rh, "SLIDE", TH_KEY, stepkey_cb, (void *) (intptr_t) 1);
		th_key_font (s_SlideK, f12);
		lv_obj_set_width (s_PanelInfo[SEQED_SWING], G->lane_w - 16);
		lv_label_set_long_mode (s_PanelInfo[SEQED_SWING], LV_LABEL_LONG_MODE_DOTS);
		th_key_font (th_key (s_Panel[SEQED_SWING], 4, ky, CX (80), kh, "-", TH_KEY, swing_cb, (void *) (intptr_t) -1), &lv_font_montserrat_24);
		s_SwingM = seg (s_Panel[SEQED_SWING], CX (92), ky, CX (500), kh, kSwingMap, -1, TH_KEY);
		th_key_font (th_key (s_Panel[SEQED_SWING], CX (600), ky, G->lane_w - 4 - CX (600), kh, "+", TH_KEY, swing_cb, (void *) (intptr_t) 1),
			     &lv_font_montserrat_24);
		// POLY (seqx E0): VOICES, ALLOCATION, UNISON with their names above, the info line over VOICES
		lv_obj_set_width (s_PanelInfo[SEQED_POLY], CX (320));
		lv_label_set_long_mode (s_PanelInfo[SEQED_POLY], LV_LABEL_LONG_MODE_DOTS);
		s_VoicesM = seg (s_Panel[SEQED_POLY], 4, ky, CX (320), kh, kVoicesMap, -1, TH_KEY);
		th_seg_font (s_VoicesM, &lv_font_montserrat_14);
		label (s_Panel[SEQED_POLY], CX (330) + 2, 2, &lv_font_montserrat_12, TH->ink, "ALLOCATION");
		s_AllocM = seg (s_Panel[SEQED_POLY], CX (330), ky, CX (196), kh, kAllocMap, 0, TH_KEY);
		th_seg_font (s_AllocM, &lv_font_montserrat_12);
		label (s_Panel[SEQED_POLY], CX (538) + 2, 2, &lv_font_montserrat_12, TH->ink, "UNISON");
		s_UniM = seg (s_Panel[SEQED_POLY], CX (538), ky, G->lane_w - 4 - CX (538), kh, kUniMap, 0, TH_KEY);
	}

	// tools
	if (oled ()) s_Tools = seg (pg, G->tools_x, G->tools_y, G->tools_w, G->tools_h, kToolRow, s_Tool, TH_KEY);
	else
	{
		s_Tools = th_seg (pg, G->tools_x, G->tools_y, G->tools_w, G->tools_h, kToolMap, TH_KEY, ctl_cb, 0);
		th_seg_gap (s_Tools, 4);
		th_seg_font (s_Tools, &lv_font_montserrat_14);
		th_seg_set (s_Tools, s_Tool);
	}
	const lv_font_t *uf = oled () ? 0 : &lv_font_montserrat_12;
	s_Undo = button (pg, G->undo_x, G->undo_y, G->undo_w, G->undo_h, "UNDO", TH_KEY, undo_cb, uf);
	s_Redo = th_key (pg, G->redo_x, G->undo_y, G->undo_w + (oled () ? 0 : 0), G->undo_h, "REDO", TH_KEY,
			 undo_cb, (void *) 1);
	if (uf) th_key_font (s_Redo, uf);

	// the selected note's card
	if (oled ())
	{
		s_Card = th_panel (pg, 0, 0, G->card_w, G->card_h, "", TH_PANEL_BARE);
		lv_obj_set_style_bg_color (s_Card, lv_color_hex (0x000000), 0);
		lv_obj_set_style_bg_opa (s_Card, LV_OPA_COVER, 0);
		lv_obj_set_style_border_color (s_Card, lv_color_hex (TH->accent_hi), 0);
		lv_obj_set_style_border_width (s_Card, 1, 0);
		s_CardText = label (s_Card, 5, 3, TH->f_body, TH->ink, "");
		s_CardSub = label (s_Card, 5, 21, mic, TH->dim, "");
		button (s_Card, G->card_w - 33, 3, 30, 30, LV_SYMBOL_TRASH, TH_KEY_DANGER, delete_cb, 0);
	}
	else
	{
		s_Card = th_panel (pg, 0, 0, G->card_w, G->card_h, "", TH_PANEL_WINDOW);
		lv_obj_set_style_border_color (s_Card, lv_color_hex (TH->accent), 0);
		lv_obj_set_style_border_width (s_Card, 1, 0);
		lv_obj_set_style_radius (s_Card, 8, 0);
		s_CardText = label (s_Card, 10, 6, &lv_font_montserrat_16, TH->win_ink, "");
		s_CardSub = label (s_Card, 10, 30, &lv_font_montserrat_12, TH->win_dim, "");
		button (s_Card, G->card_w - 52, 5, 46, 44, LV_SYMBOL_TRASH, TH_KEY_DANGER, delete_cb, &lv_font_montserrat_16);
	}
	lv_obj_add_flag (s_Card, LV_OBJ_FLAG_HIDDEN);
	// T5: the toast over the grid's top edge (an edit, or why a tap did not edit)
	s_Toast = label (pg, GRID_X + 4, GRID_Y + 3, oled () ? TH->f_body : &lv_font_montserrat_16, oled () ? TH->ink : TH->win_ink, "");
	lv_obj_set_style_bg_color (s_Toast, lv_color_hex (oled () ? 0x000000 : 0x202226), 0);
	lv_obj_set_style_bg_opa (s_Toast, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color (s_Toast, lv_color_hex (oled () ? TH->ink : TH->accent), 0);
	lv_obj_set_style_border_width (s_Toast, 1, 0);
	lv_obj_set_style_pad_all (s_Toast, oled () ? 2 : 5, 0);
	lv_obj_add_flag (s_Toast, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb (s_Grid, grid_cb, LV_EVENT_LONG_PRESSED, 0);
	if (s_Tools) th_key_text (th_seg_item (s_Tools, SEQED_DRAW), s_bLocked ? "VIEW" : "DRAW");
	if (s_ToastT) { lv_timer_delete (s_ToastT); s_ToastT = 0; }	// (a rebuild: the old toast's timer goes too)

	chord_map ();
	header_update ();
	tool_show ();
	lv_obj_add_flag (pg, LV_OBJ_FLAG_HIDDEN);
	sidecar_seqed_playhead (s_Play);
	apply_pending ();
}

// ---- host renders and tests

void sidecar_seqed_debug (int tool, int first, int pages, int top, int sel_step, int sel_pitch)
{
	s_Tool = tool;
	th_seg_set (s_Tools, tool);
	s_First = first; s_Pages = pages; s_Top = top;
	s_SelStep = sel_step; s_SelPitch = sel_pitch;
	tool_show ();
	inval_all ();
	header_update ();
	sidecar_seqed_playhead (s_Play);
}

void sidecar_seqed_debug_drag (int step, int micro, int pitch, int len)
{
	int i = s_SelStep >= 0 ? find_note (s_SelStep, s_SelPitch) : -1;
	if (step < 0 || i < 0) { g.mode = G_NONE; g.moved = 0; card_update (); lv_obj_invalidate (s_Grid); return; }
	const sidecar_seq_note_t *n = &s_V.note[i];
	g.mode = G_NOTE; g.moved = 1;
	g.step = n->step; g.micro = n->micro; g.pitch = n->pitch; g.len = n->len;
	g.nstep = step; g.nmicro = micro; g.npitch = pitch; g.nlen = len;
	card_update ();
	lv_obj_invalidate (s_Grid);
}

void sidecar_seqed_debug_key (int root, int scale, int chd, int inv, int strum)
{
	s_Root = root; s_Scale = scale; s_Chd = chd; s_Inv = inv; s_ChStr = strum;
	th_seg_set (s_RootM, root);
	th_seg_set (s_ScaleM, scale);
	th_seg_set (s_InvM, inv);
	th_seg_set (s_ChStrM, strum);
	s_bMore = 0;
	for (int i = 0; i < 10; i++) if (kChordSet[1][i] == chd && chd) s_bMore = 1;
	chord_map ();
	panel_info ();
	inval_all ();
}

void sidecar_seqed_debug_rowh (int rowh)
{
	s_RowH = clampi (rowh, G->row_min, G->row_max);
	inval_all ();
}
