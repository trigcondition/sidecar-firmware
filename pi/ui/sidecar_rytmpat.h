// pi/ui/sidecar_rytmpat.h - the Pi display's READ-ONLY view of the Rytm's own pattern (docs/TOUCH-EDITOR.md part B):
// the 12 tracks' trigs a step (velocity, accent, micro-timing, length, conditions, trigless locks), the p-locks of the
// selected track as a heatmap, the selected step's values. LVGL only; the caller gives it a decoded pattern
// (rytm_pattern.h). All calls on the LVGL thread.
#ifndef SIDECAR_RYTMPAT_H
#define SIDECAR_RYTMPAT_H

#include "rytm_pattern.h"

#ifdef __cplusplus
extern "C" {
#endif

void sidecar_rytmpat_create (void *parent, int y0, void (*back) (void));
void sidecar_rytmpat_show (int on);
int sidecar_rytmpat_shown (void);			// the page is open (the engine fetches the Rytm's pattern then)
// where the data is from, without new data ("fetching...", "3 s ago"); stale: the shown pattern may be old (a fetch
// pending or lost): the text in the warning colour
void sidecar_rytmpat_where (const char *where, int stale);
// p: copied. project/kit/where: header texts ("SHARED SET", "BREAKS", "SysEx dump 12 s ago"); index: 0..127, -1
void sidecar_rytmpat_set (const rytm_pattern_t *p, int index, const char *project, const char *kit, const char *where);
void sidecar_rytmpat_playhead (int step);		// the Rytm's step, -1 stopped
void sidecar_rytmpat_select (int track, int step);	// tests and host renders (taps do the same)
void sidecar_rytmpat_page (int page);			// 0..3, 4: all 64 steps

#ifdef __cplusplus
}
#endif

#endif
