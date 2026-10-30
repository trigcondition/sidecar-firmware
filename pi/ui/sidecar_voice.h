// pi/ui/sidecar_voice.h - the Pi display's VOICE page (rytm-mods rui1036b: UP + T4 on the Rytm): the Rytm's selected track
// as a page of its own, following the track the Rytm selects. For any machine, the analog ones too:
// - the machine's name;
// - its SRC page's P1..P8 as big dials with the Rytm's own labels (0026's 49 3);
// - a live scope and the level of the track's Overbridge stem;
// - the track's p-lock heatmap from the Rytm's own pattern (the stock dump, pi/ui/rytm_pattern.h).
// - for 6-OP FM, MULTISAMPLE and DAISYSP the machine's own panel in the scope's place (the level bar kept): 6-OP FM the
//   patch's name and its algorithm drawn as operator boxes (each one's output level filled in, its frequency under it,
//   the feedback loop); MULTISAMPLE the instrument's name, its key zones over a keyboard of its span with the keys
//   sounding lit, and the ATK DEC SUS REL envelope; DAISYSP the model (MODAL / STRING / RESON) and what STR BRI DMP
//   make of it (the resonator's modes as lines, or the string's shape). The page follows the track's machine live.
// GRANULAR and WAVETABLE tracks get the machine view (sidecar_wgview.h) instead (sidecar_ui chooses). LVGL only (builds
// on the Mac too, pi/ui/host); both themes. The caller hands it a view (sidecar_voice_set, ~15 times a second while it
// shows); it redraws only the parts whose data changed.
#ifndef SIDECAR_VOICE_H
#define SIDECAR_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

#define SVV_COLS	96		// scope columns
#define SVV_STEPS	64		// heatmap steps at most (the Rytm's pattern)
#define SVV_ZONES	32		// MULTISAMPLE's key zones shown at most
#define SVV_MODES	24		// DAISYSP's resonator modes

enum { SVV_PLAIN = 0, SVV_FM = 1, SVV_MULTI = 2, SVV_DAISY = 3 };	// the machine's panel (mode)

typedef struct
{
	int		valid;			// 0: no Rytm report yet (rui1036b's 49)
	int		track;			// 0..11
	int		machine;		// the Rytm's machine (0..52), -1 unknown
	int		sidecar;		// 1: a Sidecar machine (its voice byte), 0 analog
	char		name[16];		// "BD PLASTIC"
	unsigned char	val[8];			// P1..P8, 0..127
	char		label[8][5];		// "LEV", "TUN" .. ("" unused)
	int		ncols;
	signed char	smin[SVV_COLS], smax[SVV_COLS];	// the scope: the stem's min / max per column, -127..127
	int		level;			// the stem's peak 0..127 (dB-ish)
	int		steps;			// the pattern's steps for the track (0: no pattern fetched)
	unsigned char	locks[SVV_STEPS];	// locks on each step
	unsigned char	trig[SVV_STEPS];	// 1: a trig on that step
	int		pattern;		// 0..127, -1
	int		mode;			// SVV_*: the machine's own panel (0: the scope)
	// SVV_FM: the patch PAT picks (core/ui/extra.h FmInfo)
	char		fm_name[12];		// the DX7 voice's name ("" none)
	int		fm_alg, fm_fb;		// the algorithm 0..31 (-1 no patch), feedback 0..7
	int		fm_ncols, fm_nlev;	// the drawing's columns and levels
	signed char	fm_col[6], fm_lev[6];	// operator k's column and level (carriers on level 0)
	unsigned char	fm_mod[6];		// bit m: operator m + 1 modulates operator k + 1
	unsigned char	fm_car;			// bit k: operator k + 1 is a carrier
	signed char	fm_fb_src, fm_fb_dst;	// the feedback loop (0..5, -1 none)
	unsigned char	fm_out[6];		// output levels 0..99
	char		fm_freq[6][6];		// "1.00", "0.50", "14.0", "F440"
	// SVV_MULTI: the instrument's zones and the keys sounding
	char		ms_inst[24];		// the INST entry's name
	int		ms_ready;		// 1: its zones known (resident)
	int		ms_nz;
	unsigned char	ms_lo[SVV_ZONES], ms_hi[SVV_ZONES], ms_root[SVV_ZONES], ms_vel[SVV_ZONES];	// keys, root, layers
	unsigned char	ms_keys[16];		// bit n: key n sounding
	// SVV_DAISY: the model and its resonator's modes (MODAL / RESON) or the string (STRING)
	int		dz_model;		// 0 MODAL 1 STRING 2 RESON
	int		dz_n;			// modes below
	unsigned short	dz_ratio[SVV_MODES];	// mode frequency / the fundamental x 100
	unsigned char	dz_ring[SVV_MODES];	// how long it rings 0..255 (its Q, log)
	unsigned	gen;			// changes with anything above
} sidecar_voice_view_t;

void sidecar_voice_create (void *parent, int y0);	// hidden until shown
void sidecar_voice_show (int on);
int sidecar_voice_shown (void);
void sidecar_voice_set (const sidecar_voice_view_t *v);	// copied; cheap when gen is unchanged
// Taps the page does not act on itself (sidecar_ui's): the scope (SCOPE, following the track), MULTISAMPLE's panel (its
// instrument banks: the machine view's SOURCES). The panel says so ("TAP: BANKS").
enum { SIDECAR_VOICE_TAP_SCOPE = 0, SIDECAR_VOICE_TAP_SOURCES = 1 };
void sidecar_voice_set_tap (void (*fn) (int what));

#ifdef __cplusplus
}
#endif

#endif
