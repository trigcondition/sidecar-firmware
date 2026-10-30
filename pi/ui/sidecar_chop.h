// pi/ui/sidecar_chop.h - the Pi display's CHOP page (docs/CHOP.md "Touchscreen"; core/chop/chop.h): a sample cut into up
// to 64 slices, after the MPC Live's chop screen and the Dirtywave M8's slice list (their behaviour from their manuals
// and use, none of their code):
//   the waveform  large, the slices' markers and numbers, the selected slice lit, the playheads; drag a marker to move
//                 it (FINE: a tenth of the drag), tap a slice to select it, drag elsewhere to pan, ZOOM - / + around the
//                 selection (whole sample .. 1/64 of it); LAZY: the markers dropped so far and the sample's playhead
//   the pads      16 big pads, bank tabs A..D (slices 1..16 .. 49..64): a tap selects and auditions; LAZY: a tap drops
//                 a marker (as the Rytm's pads and trigs do)
//   the list      M8-style rows: the slice, its start and length (ms) and four of its settings in columns (a page of
//                 them: PTCH FINE GAIN REV / ATK DEC LOOP CHOK / NUDG STUT SLEN SDEC / TAPE TTIM NBR), precise numbers;
//                 the arrows move the cursor (row, column), - / + edit the value under it (x10 held: FINE off)
//   the keys      LAZY (arm; again: STOP and keep), TAP, UNDO, GRID (cycles 4 8 16 32), TRANS, ADD, DEL, RSMP; a
//                 RESAMPLE's offer: YES / NO
// LVGL only (pi/ui/host renders it); both themes (OLED, CLASSIC), both sizes. The caller hands it a view (sidecar_chop_set,
// ~10 a second while shown) built for the page's zoom window (sidecar_chop_range); it redraws only what changed.
// Every edit is a console command through the command function (chop=...: core/chop/chop.cpp command()).
#ifndef SIDECAR_CHOP_H
#define SIDECAR_CHOP_H

#ifdef __cplusplus
extern "C" {
#endif

#define SCH_COLS	512		// the outline's columns at most (the area maps them to its width)
#define SCH_SLICES	64		// core/chop kMaxSlices
#define SCH_FIELDS	15		// core/chop kFields (PTCH FINE GAIN REV ATK DEC LOOP CHOK NUDG STUT SLEN SDEC TAPE TTIM NBR)
#define SCH_HEADS	8
#define SCH_TEXT	8

enum { SCH_RS_IDLE = 0, SCH_RS_PLAYING, SCH_RS_WRITING, SCH_RS_OFFER, SCH_RS_FAILED };	// core/chop RsState

typedef struct
{
	int		valid;			// a voice plays CHOP
	int		voice;			// 0..7
	int		track;			// the Rytm track it follows (-1 none known)
	char		name[32];		// the sample's
	int		ready;			// the sample is in RAM (else LOADING / NO SAMPLE)
	unsigned	frames, rate;
	int		n;			// slices
	unsigned	start[SCH_SLICES], end[SCH_SLICES];
	int		field[SCH_SLICES][SCH_FIELDS];			// the settings' values (core/chop field_get)
	char		ftext[SCH_SLICES][SCH_FIELDS][SCH_TEXT];	// as the Rytm shows them ("+3", "300MS", "PING")
	char		fname[SCH_FIELDS][6];				// "PTCH" ..
	int		flo[SCH_FIELDS], fhi[SCH_FIELDS];		// their ranges
	unsigned	a, b;			// the frames the outline is of (the page's window, as sidecar_chop_range gave it)
	int		ncols;
	short		lo[SCH_COLS], hi[SCH_COLS];	// min / max, full scale 32767
	int		nheads;
	unsigned	head[SCH_HEADS];	// the playheads (frames)
	int		lazy, lazy_n;		// LAZY armed, its markers
	unsigned	lazy_at[SCH_SLICES];
	unsigned	lazy_pos;		// the sample's playhead since the arm
	int		slc;			// the slice the Rytm's SLC knob plays (0..)
	int		played;			// the slice the last hit played (-1)
	int		dirty;			// not written yet
	int		rs_state, rs_slice;	// RESAMPLE (SCH_RS_*)
	char		rs_row[8];
	int		lat;			// LAZY's latency, frames
	unsigned	gen;			// changes with anything above (the page redraws only then)
} sidecar_chop_view_t;

void sidecar_chop_create (void *parent, int y0);	// hidden until shown
void sidecar_chop_show (int on);
int sidecar_chop_shown (void);
int sidecar_chop_dragging (void);		// a marker is under the finger (its move is sent at the release): the page is not to be left now
void sidecar_chop_set (const sidecar_chop_view_t *v);	// copied
void sidecar_chop_range (unsigned frames, unsigned *a, unsigned *b);	// the window the page shows of a sample that long
int sidecar_chop_wanted (void);				// the voice a tap chose (-1: follow the Rytm)
void sidecar_chop_set_command (void (*fn) (const char *cmd));
// host renders and tests: the page's state set directly
void sidecar_chop_debug (int sel, int zoom, int bank, int fcol);
int sidecar_chop_sel (void);
const char *sidecar_chop_last_command (void);
// where a control is (UI coordinates; host/shot.c taps it): what 0 pad i (1..16 of the bank shown), 1 key i (LAZY TAP UNDO
// GRID TRANS ADD DEL RSMP), 2 the list's arrows / - + (UP DOWN LEFT RIGHT MINUS PLUS), 3 the list's row i shown, 4 the
// waveform at i % of its width (its middle row). 0: not shown
int sidecar_chop_hit (int what, int i, int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif
