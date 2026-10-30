// pi/ui/sidecar_song.h - the Pi display's SONG page (docs/SONG.md "Built: MVP"): SONG RECORD's read-only timeline. A
// take as lanes against a bar ruler: the patterns played (blocks), the scenes, PERF, the tempo, the 12 tracks' mutes
// (lit while muted), then a lane per parameter that moved (the Rytm's in the panel's blue print, the Sidecar's own in
// teal), the markers, and the playhead (recording / replaying). Drag scrolls, - / + zoom (1 bar .. the whole take),
// FOLLOW keeps the playhead in view. The keys: ARM / DISARM, REPLAY (a confirmation that lists what it overwrites:
// CONFIRM / CANCEL), < > the takes, MARK, CFG (the MIDI CONFIG how-to); UNDO REPLAY (song=undo) in < > 's place while
// the engine offers it (after a replay, until an edit, STOP + 30 s or a project change). LVGL only (builds on the Mac too, pi/ui/host);
// both themes. The caller hands it a view (sidecar_song_set) and runs its commands ("song=...", the console's).
#ifndef SIDECAR_SONG_H
#define SIDECAR_SONG_H

#ifdef __cplusplus
extern "C" {
#endif

#define SSG_BLOCKS	96
#define SSG_MUTES	192
#define SSG_SCENES	64
#define SSG_LANES	10
#define SSG_POINTS	160
#define SSG_TEMPO	32
#define SSG_MARKS	16
#define SSG_PPQ		96		// ticks a step
#define SSG_BAR		(SSG_PPQ * 16)

// core/song/song.h State
enum { SSG_OFF, SSG_ARMING, SSG_ARMED, SSG_REC, SSG_SAVING, SSG_CONFIRM, SSG_RESTORING, SSG_READY, SSG_REPLAYING, SSG_LOADING,
       SSG_UNDOING };

typedef struct { unsigned t0, t1; unsigned char v; } ssg_block_t;	// patterns (v), mutes (v = track), scenes (v)
typedef struct { unsigned t; unsigned short v; } ssg_point_t;		// v 0..16383 (tempo: bpm x 10)
typedef struct
{
	char		label[10];		// "SD TUN"
	int		sidecar;		// 1: the Sidecar's own
	int		n;
	ssg_point_t	p[SSG_POINTS];
} ssg_lane_t;

#define SSG_CFG_ROWS	6

typedef struct
{
	int		valid;			// 0: no engine (the page says so)
	int		state;			// SSG_*
	char		name[40];		// the take ("" none)
	char		project[24];
	char		msg[64];		// the engine's last message
	char		config[64];		// the MIDI CONFIG's problems ("" none)
	int		cfg_wrong;		// the how-to's checklist: bit k row k wrong (SSG_CFG_ROWS), -1 not read yet
	char		cfg_row[SSG_CFG_ROWS][32];	// the rows' texts (core/song config_row)
	unsigned	length;			// ticks
	unsigned	playhead;		// ticks (recording / replaying)
	unsigned	events;
	int		bpm10;			// the tempo now x 10
	int		pattern;		// 0..127, -1
	int		takes, take_i;		// on the card, the one shown (-1 not saved yet)
	int		recovered, settings;	// a recovered take; the restore writes settings X too
	int		undo;			// UNDO REPLAY offered (core/song Status::undo)
	int		nblocks, nmutes, nscenes, nperf, ntempo, nlanes, nmarks;
	ssg_block_t	blocks[SSG_BLOCKS], mutes[SSG_MUTES], scenes[SSG_SCENES];
	ssg_point_t	perf[SSG_POINTS], tempo[SSG_TEMPO];
	unsigned	marks[SSG_MARKS];
	ssg_lane_t	lane[SSG_LANES];
	unsigned	show_req;		// the engine asks for the page (a replay to confirm, song=show)
	unsigned	gen;			// changes with the lanes (the playhead apart)
} sidecar_song_view_t;

typedef void (*sidecar_song_cmd_t) (const char *cmd);

void sidecar_song_create (void *parent, int y0, sidecar_song_cmd_t cmd);	// hidden until shown
void sidecar_song_show (int on);
int sidecar_song_shown (void);
void sidecar_song_set (const sidecar_song_view_t *v);	// copied; the timeline redrawn only when gen moved
void sidecar_song_explain (int on);			// the MIDI CONFIG how-to over the timeline (CFG)
int sidecar_song_zoom (void);				// bars across the timeline (0: the whole take)

#ifdef __cplusplus
}
#endif

#endif
