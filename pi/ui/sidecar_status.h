// pi/ui/sidecar_status.h - the Pi display's SIDECAR STATUS page (UP + T10 on the Rytm when the display is up; before,
// the Rytm's own SIDECAR status page): everything the Sidecar knows about itself on one read-only page -
//   VOICES    the eight voices in the Rytm's track order, named by their track(s): the machine, the render load (a
//             share of one render core) as a bar, MUTED when the guard muted it; "x4": the sequencer's instances (E0),
//             and on the panel's title line the instances in use of the pool, one's measured cost and RAM
//   SEND FX   FX 1 / FX 2: the TYPE and PRE / POST (the kit's)
//   CPU       each core's load (core 0 is not measured), the longest block against the 12-frame budget, the longest
//             control pass, the underruns, the FIFO, the temperature
//   LINKS     the Rytm (USB, Overbridge running), the USB-C gadget, the network (Wi-Fi / Ethernet, the address), the
//             display's longest update
//   (LINKS)   on the panel's title line, the pairing code (pi/engine/paircode.h): what a new client must enter, or
//             loudly that the gate is off
//   (LINKS)   storage (docs/STORAGE.md): the SD card, the USB drive (FAT32 / exFAT, free of total, USB 3 / USB 2;
//             not mounted, ejected) and the Pi 5's NVMe SSD, each with its free space
//   VERSIONS  the Sidecar's build, Circle and LVGL, the rytm-mods level the Rytm's messages show (inferred: no build
//             number is sent); the sample library (docs/LIBRARY.md): its files, the share hashed, duplicates, missing,
//             WAIT while the Rytm plays
// LVGL only (pi/ui/host renders it); both themes, both sizes. The caller hands it a view (sidecar_status_set, about
// once a second while shown); only labels whose text changed and bars whose value changed are redrawn.
#ifndef SIDECAR_STATUS_H
#define SIDECAR_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

#define SST_VOICES	8
#define SST_CORES	4

typedef struct
{
	int		valid;			// 0: no numbers yet (the first second after boot)
	char		mach[SST_VOICES][12];	// voice v's machine ("PLAITS", "GRAN", "MULTI", "USER"; "PLAITS x4": E0's instances), "" none
	int		vload[SST_VOICES];	// voice v's render time, tenths of a percent of one render core; -1 unknown
	unsigned char	muted[SST_VOICES];	// 1: the voice guard muted it (over budget / crashed: until :unmute)
	int		load[SST_CORES];	// percent per core, -1 not measured (core 0)
	unsigned	block_max_us;		// the longest block this second (the render cores' wait included)
	unsigned	block_budget_us;	// a block's length (12 frames at 48 kHz: 250)
	unsigned	pass_max_us;		// the control core's longest pass this second
	unsigned	underruns;		// the output's steady-state ones (glitches while playing)
	// E0 (core/inst.h): the sequencer's instances - sounding / the pool's, the measured cost of one (% of a core x 100,
	// -1 none measured yet), the RAM one holds (KB)
	int		inst_used, inst_total, inst_cpu100;
	unsigned	inst_kb;
	unsigned	fifo_min, fifo_target;	// frames
	int		temp_c;			// the SoC, -1 unknown
	int		temp_warn;		// 1: the temperature warning is on (80 C up, off below 75 C): TEMP in the alert colour
	int		clock_low_mhz;		// the clock the heat lowered (thermal.h clock_low_mhz: Circle's throttle), 0: not lowered
	int		cpu_mhz;		// the ARM clock now (Settings > System; the CPU panel's title line), 0 unknown
	char		fx_type[2][12];		// "OFF" when none
	unsigned char	fx_post[2];		// 1 POST, 0 PRE
	int		rytm;			// the Rytm enumerated
	int		ob;			// the Overbridge stream runs
	unsigned	blocks;			// Overbridge blocks since boot
	char		link[12];		// "usb", "tcp"
	char		gadget[12];		// "sidecar", "midi", "" none
	int		mac;			// computer audio over USB-C: 1 on, 0 off, -1 no gadget
	int		net;			// 0 none, 1 down, 2 up
	int		wifi;			// 1 the network is Wi-Fi, 0 Ethernet
	char		ip[20], host[40];
	// the pairing code (pi/engine/paircode.h): "1234 5678", "1234 5678 FIXED" (a fixed one from cmdline.txt) or
	// "OFF (cmdline)". pair_warn: shown in the alert colour (a dev card must never look like a secured one).
	char		pair[20];
	int		pair_warn;
	unsigned	card_free_mb, card_total_mb;	// 0 / 0: no card (or not read yet)
	// storage (pi/engine/stor_vol.h's State: 0 none, 1 there but not mounted, 2 mounted, 3 ejected, 4 not on this board)
	int		usb_state;		// the first USB drive that is there (USB:, USB2:, USB3:; a mounted one first)
	int		usb_more;		// more USB drives mounted beside it
	char		usb_fs[8], usb_link[8];	// "FAT32" / "exFAT"; "USB 3", "USB 2"
	unsigned	usb_free_mb, usb_total_mb;	// free ~0u: not counted (a FAT32 without FSINFO)
	int		nvme_state;
	char		nvme_fs[8];
	unsigned	nvme_free_mb, nvme_total_mb;
	unsigned	ui_max_us;		// the display's longest update in its current 10 s window
	// the sample library (core/lib/library.h Stats): lib_state 0 off, 1 loading, 2 walking, 3 hashing, 4 ready, 5 failed
	int		lib_state;
	unsigned	lib_files, lib_hashed, lib_pending, lib_dups, lib_missing, lib_pct;
	char		lib_waiting[16];	// "PLAYING" ... while it waits, "" running
	char		build[48], circle[24];
	char		rytm_mod[32];		// "RUI1036+ (SEEN)", "" none seen
	unsigned	gen;			// changes with anything above
} sidecar_status_view_t;

void sidecar_status_create (void *parent, int y0);	// hidden until shown
void sidecar_status_show (int on);
int sidecar_status_shown (void);
void sidecar_status_set (const sidecar_status_view_t *v);	// copied; cheap when gen is unchanged

#ifdef __cplusplus
}
#endif

#endif
