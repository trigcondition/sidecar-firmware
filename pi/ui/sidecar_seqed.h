// pi/ui/sidecar_seqed.h - the Pi display's touch pattern editor for Sidecar seq tracks (docs/TOUCH-EDITOR.md): a piano
// roll over 1..8 pages of 16 steps, poly notes with velocity, length and micro-timing, chord stamps, strum, humanise,
// undo. LVGL only (it builds on the Mac too, pi/ui/host). It edits nothing itself: every gesture ends in one console
// command line ("seqed=add:..."), which core 3 applies to core/seq's pattern (pi/ui/seq_touch.h), and the caller hands
// the result back as a view (sidecar_seqed_view). In between the editor shows its own guess (the edit applied locally),
// so a tap answers at once. All calls on the thread that runs LVGL (core 0 on the Pi).
#ifndef SIDECAR_SEQED_H
#define SIDECAR_SEQED_H

#ifdef __cplusplus
extern "C" {
#endif

#define SEQED_NOTES	1024		// core/seq kMaxSteps (128) x kNotes (8)
#define SEQED_TRACKS	4		// core/seq kTracks
#define SEQED_SUB	96		// core/seq kSub: lengths and micro offsets in 1/96 step
#define SEQED_CHORDS	16		// core/poly kChords
#define SEQED_STEPS	128		// core/seq kMaxSteps
#define SEQED_LOCKS	12		// core/seq kLocks

typedef struct
{
	unsigned short	step;		// 0..length-1
	signed char	micro;		// -96..96: from the step's start (1/96 step)
	unsigned char	pitch, vel;	// MIDI note, 1..127
	unsigned char	flags;		// bit 0: the step is muted; bit 1: in NOTE EDIT's selection (U1)
	unsigned short	len;		// 1/96 step, >= 1
} sidecar_seq_note_t;

typedef struct
{
	int		valid;			// 0: no sequencer in this build (the editor says so)
	int		track;			// the Rytm track shown (0..11), -1: none of the Rytm's tracks is a seq track
	int		tracks[SEQED_TRACKS];	// the seq tracks (Rytm track numbers), -1: a free slot
	int		pattern;		// 0..127 (A01 = 0), -1 none
	char		project[24];
	char		machine[16];		// the shown track's machine ("6-OP FM"), "" unknown
	int		length, npages;		// steps (AUTO resolved) and pages of the shown track
	int		dnote, dvel, dlen;	// a new note's defaults: pitch, velocity, length (1/96 step)
	int		transport;		// 0 stop, 1 play, 2 pause
	int		bpm10;			// tempo x 10, 0 unknown
	int		undo, redo;		// levels available
	unsigned	gen;			// changes whenever the notes do (the editor redraws only then)
	char		chord[SEQED_CHORDS][8];	// core/poly's chord names ("OFF", "MAJ", ...): the CHD values
	unsigned char	locks[SEQED_STEPS];	// seq-ux: each step's parameter locks (core/seq kLocks at most): the lock lane
	unsigned char	cond[SEQED_STEPS];	// UX review T4: 1 the step has a trig condition (core/seq cond_knob), 0 none
	unsigned	show_req;		// seq-ux: the Rytm's TRK + TRIG asked for this track's SEQ (a new value: show the page)
	int		swing;			// touch2: the shown track's swing (the host's own, core/seq Track::swing), 50..80 %; 50 straight
	// E0 (seqx): the shown track's poly config (core/seq PolyCfg as it plays: the pattern's, else the track's default)
	// and what it resolves to on the track's machine ("PLAITS 4 INST 4 NOTES 2.92 %"; "" unknown)
	int		poly_voices, poly_alloc, poly_unison;	// 0 AUTO / 1..8; 0 REASSIGN 1 ROTATE 2 RESET; 0..8 (0, 1 off)
	char		poly_info[48];
	// seqtop (docs/SEQUENCER.md "Built: seqtop"): the shown track's speed / direction / random notes, the pattern's key,
	// scale, QNT and seed; per step its extras (the lane's marks, the STEP tool's state of the selected step)
	int		speed, dir;		// core/seq kSpd* (0 FOLLOW), kDir* (0 FWD)
	int		key, scale, qnt;	// the pattern's: 0..11, core/seq kScale* (0 CHR), 0 OFF 1 EDIT 2 PLAY
	int		rnd_mode, rnd_oct, rnd_wght;	// R1: 0 OFF 1 STEPS 2 TRACK; 1..4; kW*
	int		seed, seed_fixed;	// the pattern's seed (FREE: seed_fixed 0)
	unsigned char	xstep[SEQED_STEPS];	// bit 0 random notes, 1 a ratchet / arp, 2 an FX command, 3 a lock with a condition, 4 a sliding lock
	unsigned char	rt_cnt[SEQED_STEPS], rt_mode[SEQED_STEPS];	// P4 / M4: core/seq Rtrg cnt, mode
	unsigned char	fx_cmd[SEQED_STEPS];	// H1: FX column 1's command (core/seq kFx*), its value
	signed char	fx_val[SEQED_STEPS];
	unsigned char	lcond[SEQED_STEPS];	// V1: the step's first conditional lock's COND code (0 none)
	int		edit_page;		// rui1051: the Rytm's edit page of the shown track (PAGE), -1 unknown: stopped,
						// the grid's window moves to hold it when it changes
	// U1 NOTE EDIT on the Rytm (core/seq note_edit; docs/SEQ-IDEAS.md U1): on, the highlighted note (step, pitch; -1
	// none) the editor selects and follows, the selection's size, a counter that moves on every change; a note's
	// flags bit 1: it is in the Rytm's selection (framed a second time)
	int		ne_on, ne_step, ne_pitch, ne_sel;
	unsigned	ne_gen;
	int		n;
	sidecar_seq_note_t note[SEQED_NOTES];
} sidecar_seq_view_t;

typedef void (*sidecar_seqed_command_t) (const char *cmd, void *param);

// The editor's page on parent (800 x the height below the title bar), hidden until shown. cmd: where its command
// lines go (the caller queues them for core 3).
void sidecar_seqed_create (void *parent, int y0, sidecar_seqed_command_t cmd, void *param);
void sidecar_seqed_show (int on);
void sidecar_seqed_view (const sidecar_seq_view_t *v);		// the model's state (copied); cheap when gen is unchanged
void sidecar_seqed_playhead (int pos_x1000);			// the play position in steps x 1000, -1: stopped
void sidecar_seqed_touch2 (int down, int x, int y);		// a second finger (pinch), screen coordinates
void sidecar_seqed_set_rytm_cb (void (*fn) (void));		// the RYTM button (the Rytm pattern view)

// Host renders and tests: the editor's state in a known shape. SWING (touch2): the shown track's swing, set with
// "seq=swing:T:N" (T 1..12, N 50..80), shown in the header too. POLY (seqx E0): the track's poly config, set with
// "seq=poly:T:voices=N" / ":alloc=reassign|rotate|reset" / ":unison=N" (docs/SEQ-IDEAS.md E0; the Rytm's own menu later).
// seqtop: KEY is the pattern's real key (seq=key:K:S:Q) with QNT and R1's random notes (seq=rnd:T:mode=..); PLAY the
// shown track's speed and direction (seq=spd / seq=dir) and the pattern's seed (seq=seed:N|free); STEP the selected
// note's step: its ratchet / arp (seq=rtrg), FX column 1 (seq=fx), random notes (seq=rndstep), its locks' condition and
// slide (seq=lcond / seq=slide, every lock of the step).
enum { SEQED_DRAW, SEQED_CHORD, SEQED_STRUM, SEQED_HUMAN, SEQED_KEY, SEQED_SWING, SEQED_POLY, SEQED_PLAY, SEQED_STEP, SEQED_TOOLS };
void sidecar_seqed_debug (int tool, int first_page, int pages, int top_pitch, int sel_step, int sel_pitch);
void sidecar_seqed_debug_drag (int step, int micro, int pitch, int len);	// a drag in progress on the selection (-1: none)
void sidecar_seqed_debug_key (int root, int scale, int chd, int inv, int strum);
void sidecar_seqed_debug_rowh (int rowh);				// the pitch rows' height (as a pinch sets it)
// where the grid and the lane are on the screen (UI pixels) and the rows' height: the host tests aim their taps with it
void sidecar_seqed_geom (int *gx, int *gy, int *gw, int *gh, int *rowh, int *lane_y, int *lane_h);
// UX review T5: VIEW (the grid does not edit; while the Rytm plays the page opens so, a DRAW tap edits, 10 s idle VIEW
// again), a tool key's centre (UI pixels), the toast shown now ("" none)
int sidecar_seqed_locked (void);
void sidecar_seqed_tool_xy (int tool, int *x, int *y);
const char *sidecar_seqed_toast (void);
int sidecar_seqed_first_page (void);				// tests: the window's first page (rui1051)

#ifdef __cplusplus
}
#endif

#endif
