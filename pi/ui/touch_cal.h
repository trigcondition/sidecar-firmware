// pi/ui/touch_cal.h - the touch screen's 4-point calibration (Settings > Display > Calibrate, docs/pi-display.md
// "Touch calibration"): raw touch values -> the UI's pixels, per axis an offset and a scale, the raw axes swapped or not
// (an axis inverted: its edge values reversed). Plain C, no LVGL: the UI computes it (sidecar_ui.c), the display maps
// with it (sidecar_display.cpp), the kernel stores it ("touchcal=" in /.rytm/display.txt); tested on the Mac.
#ifndef TOUCH_CAL_H
#define TOUCH_CAL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
	int x0, x1;			// the raw value (of the axis that runs along the screen's x) at the left / right edge
	int y0, y1;			// the same for y, at the top / bottom edge
	int swap;			// 1: the raw axes are swapped (raw y runs along the screen's x)
} touch_cal_t;

// The 4 targets, in thousandths of the screen: top left, top right, bottom right, bottom left, 10 % in from the edges
#define TOUCH_CAL_POINTS	4
void touch_cal_target (int i, int *fx, int *fy);	// thousandths (100 or 900)

// raw[i][0..1]: the raw x/y touched at target i. 1: *out set; 0: the points do not make a screen (too close, or the
// two of an edge too far apart: a missed target)
int touch_cal_compute (const int raw[TOUCH_CAL_POINTS][2], touch_cal_t *out);

// raw -> 0..w-1, 0..h-1 (clamped)
void touch_cal_map (const touch_cal_t *c, unsigned raw_x, unsigned raw_y, int w, int h, int *x, int *y);

// "x0,x1,y0,y1,swap" (the value of touchcal=): 1 parsed; 0 not (also "off", "")
int touch_cal_parse (const char *s, touch_cal_t *out);
void touch_cal_format (const touch_cal_t *c, char *buf, int n);	// "x0,x1,y0,y1,swap"

#ifdef __cplusplus
}
#endif

#endif
