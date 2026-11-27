// pi/ui/sidecar_loop.h - the Pi display's LOOP page (spec-050 3.12, core/loop/loop.h): the six grab keys (1/2 .. 16 bars:
// a resample while a loop plays), STOP and CUT, the eight grabs as pads (newest first; a tap plays one from the next bar
// line; length, tempo, a mark when kept), KEEP for the one shown, BAL and LEV as sliders, FADE / MUTE / LIVE as keys that
// step, and the state with the bar position and the looper's last message. Every action goes back as a console line
// (loop=grab:L, loop=play:N, loop=stop[:cut:now], loop=keep:N, loop=bal:V, loop=lev:V, loop=fade:B, loop=mute:on|off,
// loop=live:off|perfN) through the command callback, as the MORPH page's do. LVGL only; both themes, every size from
// TH->w / TH->h.
#ifndef SIDECAR_LOOP_H
#define SIDECAR_LOOP_H

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_GRABS	8

typedef struct
{
	unsigned char	hb;			// length in half bars: 1 (1/2 bar), 2, 4 .. 32
	unsigned char	kept, gap, copied;	// on the card / the ring had a gap in it / the copy into the store is done
	unsigned short	bpm10;			// the tempo it was grabbed at, x 10
	unsigned	id;			// the grab's id (stable while newer ones land; the commands name it: loop=play:#ID)
} sidecar_loop_grab_t;

typedef struct
{
	int			valid;
	unsigned char		on;		// the looper exists (loopram=0 or no RAM: the page says so)
	unsigned char		state;		// 0 ring, 1 starts at the next bar line, 2 playing, 3 fading
	unsigned char		pending;	// a grab waits for its bar line's audio (while a loop plays: a resample)
	unsigned char		ring_s;		// the seconds the ring holds, 0..87
	unsigned char		bal, lev;	// 0..127
	unsigned char		fade;		// 0..4: 1/4 1/2 1 2 4 bars
	unsigned char		mute, live;	// auto-mute on; LIVE VIA 0 off, 1..12 PERF
	short			pos10, bars10;	// the loop's position in bars x 10 and its length in bars x 10 (0: none)
	signed char		playing, sel;	// the grab playing (0 newest), -1 none; the grab shown
	unsigned char		ngrabs;
	sidecar_loop_grab_t	g[SLP_GRABS];
	char			msg[28];	// the looper's last result ("PLAY FIRST", "KEPT ...")
	unsigned		gen;		// changes with anything above (a hash)
} sidecar_loop_view_t;

typedef void (*sidecar_loop_command_t) (const char *cmd);
void sidecar_loop_create (void *parent, int y0);	// hidden until shown
void sidecar_loop_set_command (sidecar_loop_command_t fn);
void sidecar_loop_show (int on);
int sidecar_loop_shown (void);
int sidecar_loop_sel (void);				// the grab shown (the page keeps it)
void sidecar_loop_set (const sidecar_loop_view_t *v);	// copied; cheap when gen is unchanged
// the centre of a control on the screen (the host shots' taps): 0..5 the grab keys, 6 STOP, 7 CUT, 8..15 the pads, 16 KEEP,
// 17 FADE, 18 MUTE, 19 LIVE, 20 the BAL slider, 21 the LEV slider; 0 when there is no such control
int sidecar_loop_key (int which, int *x, int *y);

#ifdef __cplusplus
}
#endif

#endif
