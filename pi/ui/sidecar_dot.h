// pi/ui/sidecar_dot.h - the dot-matrix display model of the OLED theme (docs/UI-THEMES.md "The grid"), without LVGL:
// the UI is drawn at half the screen's size (400x240 "dots" on 800x480) and each dot is written as a 2x2 cell: the dot's
// colour in its top-left pixel, half of it right of and below that, a quarter in the corner (sidecar_dot_expand), on the Pi in the display's flush, on the Mac in the host render. The Rytm's
// LED palette (the trig LEDs' 0x34 colours) is here too: the OLED theme takes its few colours from it.
#ifndef SIDECAR_DOT_H
#define SIDECAR_DOT_H

#include <stdint.h>
#include "sidecar_dotfont.h"

#ifdef __cplusplus
extern "C" {
#endif

// Physical pixels a dot: 2. A 1-pixel dot with a black 1-pixel gap lights a quarter of the screen and reads dark; a full
// 2x2 block loses the dot look. The cell [c c/2; c/2 c/4] keeps a visible grid of darker lines between the dots and
// lights 56 % (the old 3x3 cell [c c 0; c c 0; 0 0 0] lit 44 %). 400 x 2 = 800: no margin.
#define SIDECAR_DOT		2

// Expands the dots of src (sw x sh, src_pitch in pixels; the area's top left at dot x0,y0 of the grid) into physical
// pixels in dst (dst_pitch in pixels; dst is the area's top left): pixel (2j,2k) the dot's colour c, (2j+1,2k) and
// (2j,2k+1) c/2, (2j+1,2k+1) c/4. off: what an unlit (black) dot becomes (0: black). bright: 0..256, the lit dots
// scaled (the OLED theme's brightness). The pattern colours (sidecar_theme.h DOT_*: exact RGB565 values) become their
// checkerboards here, by the dot's grid position, so a title bar is dithered for the cost of one flat fill.
void sidecar_dot_expand (const uint16_t *src, int x0, int y0, int sw, int sh, int src_pitch, uint16_t *dst, int dst_pitch,
			 uint16_t off, int bright);

// UX review L1, the OLED theme's look: 0 STUDIO (the 2x2 cell with its finer grid, above), 1 STAGE (for a stage and an
// angle: each dot its whole cell, no grid lines; every grey a step lighter, v -> 0x30 + v x 0xD0 / 0xFF, so the darkest grey
// is #303030 and the faint text (#7C7C7C -> #9A9A9A) reads at an angle)
void sidecar_dot_look (int stage);
int sidecar_dot_get_look (void);

// The Rytm's trig LED palette (rytm-mods 0026 rui1026, core/ui/extra.h): 0..111, ramps of 16 (red 0, green 16, blue 32,
// amber 48, cyan 64, magenta 80, white 96), entry 0 of each ramp off. The RGB is the one sim/seq_sim.py renders.
uint32_t sidecar_led_rgb (int c);		// 0xRRGGBB
enum { LED_RED = 0, LED_GREEN = 16, LED_BLUE = 32, LED_AMBER = 48, LED_CYAN = 64, LED_MAGENTA = 80, LED_WHITE = 96 };
// core/seq's trig LED colours (seq.cpp): a note blue 47, a chord cyan 79, the play position white 111, held amber 63
enum { LED_NOTE = 47, LED_CHORD = 79, LED_PLAY = 111, LED_HELD = 63, LED_REC = 15, LED_OK = 31 };

#ifdef __cplusplus
}
#endif

#endif
