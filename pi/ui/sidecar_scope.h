// pi/ui/sidecar_scope.h - the Pi display's SCOPE page (UP + T9 on the Rytm when the display is up; before, the Rytm's
// own SCOPE page): an oscilloscope of one Overbridge channel - the Rytm's selected track (TRACK: follows the Rytm),
// MAIN, a track's stem, IN L / R (or the FX return) - with a timebase (43 ms .. 2.7 s a screen; the short ones
// triggered on a rising crossing, the long ones a sweep), a gain (AUTO or x1 .. x64) and FREEZE, over the level meters
// of all eleven channels (MAIN as L / R; a tap on a meter shows its channel). The same tap and timebases as the Rytm's
// SCOPE page (core/ui/page_scope.cpp), the meters the recorder's (pi/engine/recorder.h meter_peaks, the REC page's).
// LVGL only (pi/ui/host renders it); both themes, both sizes. The caller hands it a view (sidecar_scope_set, <= 15
// a second while shown) built for the page's choices (sidecar_scope_source / timebase); FREEZE is the page's own.
// Cost: the trace's area is redrawn only when a new trace comes (not frozen), each meter's strip only when its lit
// segments or hold move.
#ifndef SIDECAR_SCOPE_H
#define SIDECAR_SCOPE_H

#ifdef __cplusplus
extern "C" {
#endif

#define SSC_COLS	256		// the trace's columns (the area maps them to its width)
#define SSC_SOURCES	11		// 0 MAIN, 1..8 the Rytm's tracks' stems in track order (BD SD RS/CP BT LT MT/HT CH/OH CY/CB), 9 / 10 IN L / R
#define SSC_CH		12		// the meters' channels (the Overbridge order): MAIN L, MAIN R, the 8 stems, IN L, IN R
#define SSC_TBS		7		// timebases
#define SSC_FS		8388607u	// a meter peak's full scale (24 bit, as the recorder's)
#define SSC_TRIG_TBS	2		// timebases below this are triggered, the rest sweep

typedef struct
{
	int		valid;			// 0: no Overbridge audio yet (no Rytm): the page says so
	int		src;			// the source the trace is of (0..10)
	int		track;			// the Rytm's selected track (0..11), -1 unknown (TRACK's source and name)
	int		tb;			// the timebase the trace is in
	int		ncols;			// columns filled (fewer than SSC_COLS: from the left)
	int		cursor;			// a sweep: the next column written (-1 triggered)
	short		lo[SSC_COLS], hi[SSC_COLS];	// min / max, full scale 32767
	unsigned	peak[SSC_CH];		// the meters: each channel's peak since the last view, 0..SSC_FS
	char		name[SSC_SOURCES][8];	// "MAIN", "BD", .. "IN L" (or "FX L" while the IN pair carries the FX return)
	unsigned	gen;			// changes with the trace (the area redraws only then)
} sidecar_scope_view_t;

void sidecar_scope_create (void *parent, int y0);	// hidden until shown
void sidecar_scope_show (int on);
int sidecar_scope_shown (void);
void sidecar_scope_set (const sidecar_scope_view_t *v);	// copied; meters advance (rise at once, fall 24 dB/s, hold 1.5 s)
// The page's choices (the provider builds the trace for them): the source (0..10; -1 TRACK: the Rytm's selected track),
// the timebase (0..SSC_TBS-1: 43 ms .. 2.7 s a screen). FREEZE holds the trace (the meters go on).
int sidecar_scope_source (void);
int sidecar_scope_timebase (void);
int sidecar_scope_frozen (void);
const char *sidecar_scope_tb_name (int tb);		// "43MS" .. "2.7S"
int sidecar_scope_tb_entries (int tb);			// the tap's entries (8 frames each) a column: 1 .. 64
// host renders and tests: the choices set directly
void sidecar_scope_debug (int source, int tb, int gain, int frozen);
int sidecar_scope_gain (void);				// 0 AUTO, k: x 2^(k-1)

#ifdef __cplusplus
}
#endif

#endif
