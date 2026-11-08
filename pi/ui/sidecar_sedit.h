// pi/ui/sidecar_sedit.h - the Pi display's SAMPLE EDIT page (core/samp/sedit.h): the touchscreen's view of the sample
// editor the Rytm's SAMPLE EDIT page shows too (one edit, two views):
//   the waveform  large, the window's outline, the region [a, b) lit, its start and end markers; drag a marker to move
//                 it (sent at the release), drag elsewhere to pan, ZOOM - / + halve or double the window
//   the keys      one a function (CROP NORMALISE TRIM FADE IN FADE OUT REVERSE GAIN +3 GAIN -3: a tap applies it to the
//                 region), UNDO, PLAY (the region once on the track), SAVE (asks SAVE + USE, only for an edit opened from
//                 a track, or SAVE), CLOSE (asks before it drops an edit not saved)
// LVGL only (pi/ui/host renders it); both themes (OLED, SIDECAR), all sizes. The caller hands it a view (sidecar_sedit_set,
// ~10 a second while shown); it redraws only what changed. Every action is a console command through the command
// function (sedit=...: core/samp/sedit.cpp command()).
#ifndef SIDECAR_SEDIT_H
#define SIDECAR_SEDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define SSE_COLS	1024		// core/samp/sedit kCols: the outline's columns

enum { SSE_ST_CLOSED = 0, SSE_ST_LOADING, SSE_ST_READY, SSE_ST_BUSY, SSE_ST_SAVING, SSE_ST_ERROR };	// sedit::State
enum { SSE_FN_CROP = 0, SSE_FN_NORM, SSE_FN_TRIM, SSE_FN_FADE_IN, SSE_FN_FADE_OUT, SSE_FN_REV, SSE_FN_GAIN_UP, SSE_FN_GAIN_DN, SSE_FN_N };	// sedit_dsp.h Fn
enum { SSE_K_ZOOM_OUT = 0, SSE_K_ZOOM_IN, SSE_K_PLAY, SSE_K_UNDO, SSE_K_SAVE, SSE_K_CLOSE, SSE_K_N };	// the page's other keys
enum { SSE_D_USE = 0, SSE_D_OK, SSE_D_CANCEL, SSE_D_N };	// the question's keys: SAVE + USE; SAVE / DISCARD; CANCEL

// A flat copy of sedit::View (seditview_pi.cpp fills it)
typedef struct
{
	int		valid;			// an edit is open (st not SSE_ST_CLOSED)
	int		st;			// SSE_ST_*
	char		name[48], msg[32];	// the source's file name; the editor's last refusal, error or note
	unsigned	frames, rate, a, b, wa, wb;	// the length; the region [a, b); the window [wa, wb) the outline is of
	int		ch, bits, fn, zoom, undo, dirty, has_track, track, progress, ncols;
	short		lo[SSE_COLS], hi[SSE_COLS];	// the outline: min / max of a column, full scale 32767
	int		playhead;		// the preview's frame (-1 none)
	unsigned	gen;			// changes with anything above (the page redraws only then)
} sidecar_sedit_view_t;

void sidecar_sedit_create (void *parent, int y0);	// hidden until shown
void sidecar_sedit_show (int on);
int sidecar_sedit_shown (void);
int sidecar_sedit_dragging (void);		// a marker is under the finger (its move is sent at the release): the page is not to be left now
void sidecar_sedit_set (const sidecar_sedit_view_t *v);	// copied
void sidecar_sedit_set_command (void (*fn) (const char *cmd));
// host renders and tests
const char *sidecar_sedit_last_command (void);
// where a control is (UI coordinates; host/shot.c taps it): what 0 the start (i 0) or end (1) marker, 1 function key i
// (SSE_FN_*), 2 key i (SSE_K_*), 3 the question's key i (SSE_D_*), 4 the waveform at i % of its width (its middle row).
// 0: not shown
int sidecar_sedit_hit (int what, int i, int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif
