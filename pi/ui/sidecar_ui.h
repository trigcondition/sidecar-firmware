// pi/ui/sidecar_ui.h - the Pi display's screen (docs/pi-display.md): LVGL only, no Circle, so the same file builds
// in the Pi kernel and on the Mac (pi/ui/host: a PNG of the layout). The caller owns LVGL (lv_init, the display,
// the input device, lv_timer_handler) and calls these from that one thread.
#ifndef SIDECAR_UI_H
#define SIDECAR_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#define SIDECAR_UI_TRACKS	8
#define SIDECAR_UI_CORES	4
#define SIDECAR_UI_SENDS	12		// the Rytm's tracks, each with a send level to FX 1 and FX 2

typedef struct
{
	int		rytm;				// 1: the Rytm is enumerated
	unsigned	blocks;				// Overbridge blocks seen
	unsigned	underruns;
	int		load[SIDECAR_UI_CORES];		// percent per core, -1: not known
	const char	*track[SIDECAR_UI_TRACKS];	// voice/machine names, 0: "-"
	const char	*note;				// a free line at the bottom (build, link), 0: none
	// the settings page's state (the web page's /status): re-read by the caller, so a change from the web page, the
	// serial console or this page shows here
	int		notes;				// MIDI notes on
	int		out_tracks;			// 0: out=main, 1: out=tracks
	int		mac;				// computer audio: 0/1, -1: no USB-C Sidecar gadget
	int		drone;				// -1 off, 0..7 one voice, 8 all
	unsigned	fifo;				// FIFO target, frames
	int		web;				// the web page runs (cmdline web=, not switchable at run time)
	char		fx_type[2][12];			// FX 1 / FX 2 TYPE names ("OFF" when none)
	unsigned char	fx_post[2];			// 1 = returned POST (0/1, clean), 0 = PRE (2/3, through the analog dist + comp)
	unsigned char	send[2][SIDECAR_UI_SENDS];	// 0..127: track t's send level to FX 1 / FX 2 (the kit's)
	char		ip[20], host[40];		// "169.254.61.60", "sidecar.local" ("" until mDNS runs)
	char		link[12], gadget[12];		// "usb", "sidecar"
	char		build[48], circle[24];		// "0.1.0 (1332761)" (core/version.h), "Circle 51.1.1"
	int		touch_hidden;			// 1: the home page's touch test is hidden (touchtest=0, /.rytm/display.txt)
	int		screensaver;			// minutes without a touch until black (0: never), -1: not known
	int		brightness;			// 10..100 %: the picture dimmed (the panel has no backlight control); 0: not known
	// the user machines (core/user/um.h UiView): the Rytm's project and U1..U5 as the Rytm shows them
	char		project[24];			// "" until the engine has published
	char		uslot[5][12];			// "ACID", "---" empty, "!ACID" missing, "ACID..." loading
	unsigned char	uslot_state[5];			// 0 empty 1 ready 6 loading, else a problem (rmp_slot_state)
	int		rec;				// the recorder: 0 off, 1 armed (waits for PLAY), 2 recording
	unsigned	rec_secs;			// the take's length so far
	int		ui_style;			// the display's theme (sidecar_theme.h THEME_*: 0 OLED, 1 CLASSIC), -1: not known
	int		ui_look;			// UX review L1: the OLED look, 0 STUDIO 1 STAGE (sidecar_dot_look), -1: not known
	unsigned	poly;				// UX review S1: bit v - voice v plays a poly track (a POLY badge on HOME)
	int		pattern;			// UX review T1: the Rytm's pattern 0..127 (-1 not known) and transport (0 stop,
	int		transport;			// 1 play, 2 pause): HOME's NOW panel
	int		touch_cal;			// 1: the touch is calibrated (touchcal=, Settings > Display), 0: the default
	int		temp_c;				// the SoC's temperature, 0: not known
	int		temp_warn;			// 1: the temperature warning is on (80 C up, off below 75 C: pi/engine/thermal.h): a banner
	int		clock_low_mhz;			// the ARM clock Circle's throttle lowered it to (thermal.h clock_low_mhz), 0: not lowered
	int		cpu_mhz;			// the CPU clock setting (Settings > System: 1500 / 1200 / 1000), 0: not known
} sidecar_ui_status_t;

// A command line as the serial console takes it (":notes=1", without the colon): the caller runs it (CConsole::Command)
// and re-reads its state shortly after. Called from sidecar_ui's event handlers, on the thread that calls LVGL.
typedef void (*sidecar_ui_command_t) (const char *cmd, void *param);
void sidecar_ui_set_command (sidecar_ui_command_t fn, void *param);
// 0 home, 1 settings/Sound, 2 settings/Send FX, 3 settings/About, 4 SEQ (sidecar_seqed.h), 5 the Rytm pattern
// (sidecar_rytmpat.h), 6 settings/Display, 7 the WAVETABLE / GRANULAR machine view (sidecar_wgview.h; the host render shows each)
// 8 VOICE: the Rytm's selected track (sidecar_voice.h; rui1036b UP + T4 on the Rytm), 9 INSERT FX (sidecar_insfx.h;
// Settings > Send FX > Inserts, or UP + T5 on the Rytm), 10 SONG (sidecar_song.h; SONG RECORD; UP + T6),
// 11 REC (sidecar_rec.h: the Overbridge recorder; UP + T7 on the Rytm, or a tap on the home page's REC status),
// 12 SCOPE (sidecar_scope.h; UP + T9, or a tap on VOICE's scope), 13 SIDECAR STATUS (sidecar_status.h; UP + T10, or a tap
// on the home page's ENGINE / Status panel), 14 PROJECT (sidecar_project.h: project bundles; a tap on
// the home page's PROJECT panel), 15 POOL (sidecar_wgview.h's POOL page: the three pool lists; Settings > Pool, or the
// machine view's ALL POOLS), 16 SERVICE (settings), 17 CHOP (sidecar_chop.h: a CHOP voice's page; VOICE on a CHOP track)
void sidecar_ui_show (int page);
// UX review N3: the Rytm's UP held alone: the UP + T map over the screen (0: gone); cheap when unchanged
void sidecar_ui_combo_map (int on);
int sidecar_ui_combo_map_shown (void);
const char *sidecar_ui_banner (void);				// the warning banner's text while it is shown (the supply's, or the temperature's), else 0 (tests)
int sidecar_ui_page (void);					// the page shown
int sidecar_ui_nav_lit (void);					// the navigation key lit for it (-1: none; tests)
// rui1052: navigation key i (0..7 top to bottom), its page's name in the device's table and the caption painted on the
// key ("T1"; "" when the device has no key for that page) - the tests hold the two to the same order (pi/ui/host/shot.c)
const char *sidecar_ui_nav_page (int i);
const char *sidecar_ui_nav_key (int i);
unsigned sidecar_ui_taps (void);				// the touch test's TAP count (tests)
int sidecar_ui_touch_test_shown (void);				// HOME shows the touch test (until the first tap)

// The caller creates LVGL's display at the screen's (physical) size; sidecar_ui_create then sets the theme's resolution
// (OLED: half of it, 400x240 dots on 800x480: docs/UI-THEMES.md) and builds the pages. Each flush then carries UI
// pixels: the caller expands them by sidecar_ui_dot () (sidecar_dot_expand, at sidecar_ui_origin ()) and divides its
// pointer's coordinates the same way (sidecar_ui_to_ui).
void sidecar_ui_create (void);					// on lv_screen_active ()
// The theme (THEME_OLED / THEME_CLASSIC): the screen is rebuilt and redrawn at once (core 0's loop only: a few ms of
// object creation plus one full redraw; nothing on the audio cores). A tap on Settings > Display > UI style calls it
// and sends "uistyle=oled|classic" (stored in /.rytm/display.txt); a status with another ui_style calls it too.
void sidecar_ui_style (int style);
int sidecar_ui_get_style (void);
int sidecar_ui_dot (void);					// physical pixels a UI pixel now: 1 or SIDECAR_DOT
void sidecar_ui_origin (int *x, int *y);			// where the UI's pixel 0,0 is on the screen (the dot grid centred)
void sidecar_ui_to_ui (int *x, int *y);				// screen -> UI coordinates (clamped)
int sidecar_ui_bright (void);					// OLED: the dots' brightness for the expansion, 0..256
								// (CLASSIC: 256, a black layer dims the picture)
void sidecar_ui_status (const sidecar_ui_status_t *s);		// labels change only when their text does
void sidecar_ui_touch (int pressed, int raw_x, int raw_y, int x, int y);	// the calibration readout
// Settings > Display > CALIBRATE (touch_cal.h): the 4 crosses over everything; each release on one takes its raw values,
// then "touchcal=X0,X1,Y0,Y1,SWAP" is sent (RESET: "touchcal=off"). The step 0..3 shown, -1: not calibrating (tests)
void sidecar_ui_touch_cal_start (void);
int sidecar_ui_touch_cal_step (void);
// The picture's brightness, 10..100 %: a black layer over everything (software dimming: the 5" HDMI (H) panel's
// backlight has no software control without a hardware change). Applied at once; 100: no layer.
void sidecar_ui_dim (int percent);

#ifdef __cplusplus
}
#endif

#endif
