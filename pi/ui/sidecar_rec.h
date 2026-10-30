// pi/ui/sidecar_rec.h - the Pi display's REC page: the Overbridge multitrack recorder (pi/engine/recorder.h; the web
// page's Record tab and the Rytm's own REC page drive the same one). A transport row (REC / STOP with a record mark,
// ARM: PLAY on the Rytm starts a take and STOP ends it, the elapsed time as segment digits, the take, 48 kHz and the
// bit depth, the destination's free space and the time it holds at the selection - the drive named when it is not the
// SD: "USB 57 GB"), then the channels: the Rytm's eight stems,
// the Sidecar's eight voices (named for their tracks, in the Rytm's track order, beside the stem of the same track),
// the mix (MASTER, MAIN L / R, IN L / R or the FX return, FX 1 / FX 2): a tap arms or disarms a channel (idle only),
// each with a live meter (peak, a held peak, a clip lamp held 3 s). TAKES swaps the channels for the takes, newest
// first (name, length, size, files; a vertical drag scrolls them), with a delete behind a confirmation. REC or ARM
// with a selection that needs more than ~70 % of the destination's write speed (warn_pct) asks first. LVGL only (pi/ui/host renders it); both themes,
// both sizes. The caller hands it a view (sidecar_rec_set, <= 25 a second while shown) and runs its commands (the
// console's "rec=..." lines).
#ifndef SIDECAR_REC_H
#define SIDECAR_REC_H

#ifdef __cplusplus
extern "C" {
#endif

#define SRV_CH		23		// the recorder's sources (core/rec/rec_names.h: 0 MASTER, 1..12 IN, 13..20 voices, 21/22 FX)
#define SRV_TAKES	64		// the newest takes the page lists (scrolled: a few rows show at a time)
#define SRV_NAME	64		// a take's id with its volume (core/rec/rec_names.h kTakeMax)
#define SRV_FS		8388607u	// a peak's full scale (24 bit)

enum { SRV_IDLE, SRV_ARMED, SRV_REC, SRV_SAVING };	// recorder.h's states
enum { SRV_PL_NONE, SRV_PL_PLAYING, SRV_PL_PAUSED };	// a take's playback (pi/engine/rec_play.h)

typedef struct
{
	char		name[SRV_NAME];	// its id: "jam_2026-10-27_1432" on the SD, "USB:jam_take-004" on a drive
	unsigned	secs;			// its length
	unsigned	kb;			// its files together
	int		files;
} srv_take_t;

typedef struct
{
	int		valid;			// 0: recording is off (cmdline recbuf=0, no card): the page says so
	int		state;			// SRV_*
	int		arm_mode;		// the take was started (is waited for) by PLAY: STOP ends it
	unsigned	bits, rate;		// 16 / 24, 48000
	unsigned	secs;			// this take's length, or the last one's
	char		take[SRV_NAME];	// "" none yet
	char		error[64];		// the recorder's (or a refused command's) error, "" none
	unsigned	free_mb, total_mb;	// the destination's (the card, or the drive in vol)
	char		vol[8];			// the destination: "SD" (or "" : the SD), "USB", "USB2", "USB3", "NVME" (rec=vol:)
	int		vol_missing;		// that drive is not mounted now (REC / ARM are refused: shown)
	int		free_unknown;		// its free space not counted (a FAT32 without FSINFO): "?", not 0
	unsigned	need_kbps;		// the selection's data rate
	unsigned	speed_kbps;		// the destination's write speed: stor=bench's, else a default for its kind
	int		speed_measured;		// 1: measured (stor=bench), 0: the default
	unsigned	warn_pct;		// 0: the selection fits; else what it needs, in % of speed_kbps (REC / ARM ask first)
	unsigned	drops;			// blocks the card fell behind on (written as silence)
	unsigned	mask;			// bit id: the channel is recorded
	char		name[SRV_CH][8];	// shown as is ("MASTER", "BD", "CH/OH", "FX L")
	unsigned	peak[SRV_CH][2];	// since the last view, 0..SRV_FS; [1]: R of a stereo source
	int		ntakes;			// -1: not read yet
	srv_take_t	takes[SRV_TAKES];	// newest first
	unsigned	takes_gen;		// changes with the list
	int		pl_state;		// SRV_PL_*: the take playing (to the Rytm's MAIN) or paused, wherever the page was left
	char		pl_take[SRV_NAME];	// its name
	unsigned	pl_pos_ms, pl_len_ms;	// elapsed, length
} sidecar_rec_view_t;

typedef void (*sidecar_rec_cmd_t) (const char *cmd);

void sidecar_rec_create (void *parent, int y0, sidecar_rec_cmd_t cmd);	// hidden until shown
void sidecar_rec_show (int on);
int sidecar_rec_shown (void);
void sidecar_rec_set (const sidecar_rec_view_t *v);	// copied; meters advance (rise at once, fall 24 dB/s)
void sidecar_rec_takes (int on);			// the takes instead of the channels (the TAKES key)
int sidecar_rec_takes_shown (void);
int sidecar_rec_stereo (int id);			// 1: MASTER, FX 1, FX 2

#ifdef __cplusplus
}
#endif

#endif
