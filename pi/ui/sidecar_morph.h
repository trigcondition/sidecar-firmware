// pi/ui/sidecar_morph.h - the Pi display's MORPH page (spec-050 2.10, core/mod/scene.h): the twelve scenes as pads in two
// rows (a tap selects; a tap on the A or B key, then a pad, assigns that end), the selected scene's locks as a list (a
// slider and a delete key a row, scrolled), and the crossfader across the bottom (drag: absolute). Every edit goes
// back as a console line (scene=sel:N, scene=ab:A:B, scene=fader:V, scene=lock:N:NAME:V, scene=unlock:N:NAME) through
// the command callback, as the MASTER FX page's do. LVGL only; both themes, every size from TH->w / TH->h.
#ifndef SIDECAR_MORPH_H
#define SIDECAR_MORPH_H

#ifdef __cplusplus
extern "C" {
#endif

#define SMO_SCENES	12
#define SMO_ROWS	16

typedef struct
{
	char		name[16];		// "V1:P3", "RYTM:PERF12" (16: room for "RT12:FRQ"-like names to come)
	char		label[16];		// as the screens name it: "V1:HRM" (the voice's machine), "RYTM:PERF12"
	char		value[8];		// as the Rytm's MORPH page shows it: "64", "50.5", "OFF"
	unsigned char	v;			// the slider's 0..127 over lo..hi
	short		lo, hi;			// the target's range in whole units (RYTM:SCN 0..12, a voice's P 0..127)
} sidecar_morph_row_t;

typedef struct
{
	int			valid;
	unsigned short		fader;		// 0..16383 as set (A at 0)
	signed char		a, b, sel;	// 0..11, -1 none
	unsigned char		nlocks[SMO_SCENES];
	unsigned char		top;		// the first row's index in the selected scene's locks
	unsigned char		nrows;		// rows filled from top (at most SMO_ROWS)
	unsigned char		edit;		// the Rytm's EDIT (hold a scene key and turn a knob on any Sidecar page)
	unsigned char		locked;		// the scene file is newer than this build: not saved
	sidecar_morph_row_t	row[SMO_ROWS];
	unsigned		gen;		// changes with anything above (a hash)
} sidecar_morph_view_t;

typedef void (*sidecar_morph_command_t) (const char *cmd);
void sidecar_morph_create (void *parent, int y0);	// hidden until shown
void sidecar_morph_set_command (sidecar_morph_command_t fn);
void sidecar_morph_show (int on);
int sidecar_morph_shown (void);
int sidecar_morph_top (void);				// the list's scroll: the first row the page wants filled
void sidecar_morph_set (const sidecar_morph_view_t *v);	// copied; cheap when gen is unchanged

#ifdef __cplusplus
}
#endif

#endif
