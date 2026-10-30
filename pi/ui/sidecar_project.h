// pi/ui/sidecar_project.h - the Pi display's PROJECT page (project bundles, docs/BUNDLES.md "PROJECT pages"), opened
// by a tap on the home page's PROJECT panel. The card's projects (the Rytm's current one marked *, imported ones imp.,
// the size of their Sidecar files, last used) with the selected one's details; the destination / source picker (the
// card or a mounted USB stick, with free space) and EJECT; per project EXPORT (to the card's /Bundles or the stick's
// /SIDECAR) and DELETE (a confirmation whose DELETE stays grey for a second; never the current project); IMPORT lists
// the bundles on the card or the stick, a tap picks one, IMPORT IT checks every byte and shows the summary (the name it
// imports as, new / reused MB, the machines with a tick each, the Rytm step), then the progress (the phase, a bar, MB
// of MB, WAITING (PLAYING)), the done screen with "save your Rytm project as NAME 2", or the error and what to do.
// LVGL only (pi/ui/host renders it); both themes, both sizes. The caller hands it a view (sidecar_project_set, a few
// times a second while it shows: core/bundle/feed.h's Feed) and runs its commands (the console's "bundle=..." lines).
// 2026-10-27: TRASH lists the project trash (a project's Sidecar files a CLEAR / DELETE / SAVE TO / PASTE on the Rytm or
// a DELETE here moved there), RESTORE puts the selected one back under its slot ("bundle=restore:<id>"; refused when the
// slot has files or is the Rytm's current project - the reason on the status line).
#ifndef SIDECAR_PROJECT_H
#define SIDECAR_PROJECT_H

#ifdef __cplusplus
extern "C" {
#endif

#define SPJ_PROJECTS	48
#define SPJ_BUNDLES	32
#define SPJ_VOLS	3
#define SPJ_MACH	8
#define SPJ_TRASH	16

// core/bundle/bundle.h's Phase and Job
enum { SPJ_IDLE, SPJ_FLUSH, SPJ_COLLECT, SPJ_HASH, SPJ_WRITE, SPJ_SERVE, SPJ_VERIFY, SPJ_PLAN, SPJ_READY, SPJ_BACKUP,
       SPJ_STAGE, SPJ_COMMIT, SPJ_LINK, SPJ_DELETE, SPJ_DONE, SPJ_FAILED, SPJ_CANCELLED };
enum { SPJ_JOB_NONE, SPJ_JOB_EXPORT, SPJ_JOB_IMPORT, SPJ_JOB_DELETE };
// a machine's action in an import (bundle.h PkgAction)
enum { SPJ_PKG_INSTALL, SPJ_PKG_REUSE, SPJ_PKG_CONFLICT, SPJ_PKG_SIDE, SPJ_PKG_LEFTOUT, SPJ_PKG_NOTPI, SPJ_PKG_REFUSED, SPJ_PKG_CHECK };

typedef struct { char key[24], rytm[24], date[20]; unsigned files, used; unsigned long long bytes; int current, imported; } spj_project_t;
typedef struct { char name[96], path[160], date[20]; unsigned long long size; int incomplete; } spj_bundle_t;
typedef struct { char drive[8], name[8]; unsigned long long free, total; int free_known; } spj_vol_t;
typedef struct { int slot, action; char name[12], ver[12]; } spj_mach_t;
typedef struct { unsigned id; char key[24], name[24], why[12], date[24]; int items; } spj_trash_t;	// the project trash

typedef struct
{
	int		valid;			// 0: no bundle engine (the page says so)
	unsigned	gen;			// moves with the lists
	int		reading;		// a list asked for and not read yet
	int		nproj;
	spj_project_t	proj[SPJ_PROJECTS];
	int		nvol;
	spj_vol_t	vol[SPJ_VOLS];
	char		src[8];			// the bundles' source: "" the card, else a stick's drive
	int		nbund;
	spj_bundle_t	bund[SPJ_BUNDLES];
	unsigned long long card_free;		// 0 unknown
	unsigned	eject_n;		// moves with each eject's answer
	char		eject_msg[64];		// "USB: ejected: safe to remove", or why not
	// the job (bundle::Status)
	unsigned	seq;
	int		job, phase;
	unsigned long long done, total, bytes;
	char		key[24], file[160], msg[200], waiting[16], job_vol[8];
	// an import's summary (READY) / done screen (DONE)
	int		have_plan;
	char		plan_name[24], plan_from[24], plan_step[300], plan_rytm[64];
	int		plan_clash;
	int		plan_slot;		// slot mode: the slot the import goes to (0 name mode); clash: it has Sidecar files
	int		plan_pick;		// slot mode, every slot taken: no default (the web page picks one)
	unsigned long long plan_new, plan_reused;
	unsigned	plan_write, plan_reuse, plan_samples, plan_songs, plan_patterns, plan_missing;
	int		nmach;
	spj_mach_t	mach[SPJ_MACH];
	// 2026-10-27: the project trash (core/bundle/prjslot.h), the newest first; the slot jobs' last outcome
	int		ntrash;
	spj_trash_t	trash[SPJ_TRASH];
	char		prj_last[48];
} sidecar_project_view_t;

typedef void (*sidecar_project_cmd_t) (const char *cmd);

void sidecar_project_create (void *parent, int y0, sidecar_project_cmd_t cmd);	// hidden until shown
void sidecar_project_show (int on);	// on: the lists are asked for again (bundle=list)
int sidecar_project_shown (void);
void sidecar_project_set (const sidecar_project_view_t *v);	// copied; labels redrawn only where they changed

// tests (pi/ui/host): where a control is (its centre, UI coordinates); 0: not there now
enum { SPJ_HIT_ROW, SPJ_HIT_KEY, SPJ_HIT_DEST, SPJ_HIT_EJECT, SPJ_HIT_DLG, SPJ_HIT_MACH };
int sidecar_project_hit (int what, int i, int *x, int *y);
int sidecar_project_listing (void);	// 0 the projects, 1 the bundles, 2 the trash (2026-10-27: TRASH, its RESTORE)
int sidecar_project_dialog (void);	// what the dialog shows: 0 none, 1 progress, 2 summary, 3 done, 4 failed, 5 cancelled, 6 delete
int sidecar_project_selected (void);	// the selected row's index (projects or bundles)

#ifdef __cplusplus
}
#endif

#endif
