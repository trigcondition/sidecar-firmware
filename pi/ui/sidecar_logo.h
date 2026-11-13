// pi/ui/sidecar_logo.h - the "Docked" logo (the web app's, docs/UI-THEMES.md) as a table, for the SIDECAR theme
// (th_paint_logo) and its boot screen (sidecar_splash.c): the 7x7 dot grid at 4 + 4.6 g, 6 + 4.6 g of a 46 x 40 box,
// the dots within 15.5 of (17.8, 19.8), each 0.5 + 1.7 (1 - d / 15.5) in radius and coloured by its distance (0 coral at
// the centre, 1 magenta, 2 the accent, 3 lavender at the edge), and a solid accent disc of 7.5 at (37, 27).
// Positions x10, radii x100, precomputed.
#ifndef SIDECAR_LOGO_H
#define SIDECAR_LOGO_H

#include <stdint.h>

#define SIDECAR_LOGO_N	37
static const struct sidecar_logo_dot { int16_t x, y, r; uint8_t c; } kLogo[SIDECAR_LOGO_N] = {
	{132, 60, 60, 3}, {178, 60, 69, 3}, {224, 60, 60, 3}, {86, 106, 77, 3}, {132, 106, 107, 2}, {178, 106, 119, 2},
	{224, 106, 107, 2}, {270, 106, 77, 3}, {40, 152, 60, 3}, {86, 152, 107, 2}, {132, 152, 149, 1}, {178, 152, 170, 1},
	{224, 152, 149, 1}, {270, 152, 107, 2}, {316, 152, 60, 3}, {40, 198, 69, 3}, {86, 198, 119, 2}, {132, 198, 170, 1},
	{178, 198, 220, 0}, {224, 198, 170, 1}, {270, 198, 119, 2}, {316, 198, 69, 3}, {40, 244, 60, 3}, {86, 244, 107, 2},
	{132, 244, 149, 1}, {178, 244, 170, 1}, {224, 244, 149, 1}, {270, 244, 107, 2}, {316, 244, 60, 3}, {86, 290, 77, 3},
	{132, 290, 107, 2}, {178, 290, 119, 2}, {224, 290, 107, 2}, {270, 290, 77, 3}, {132, 336, 60, 3}, {178, 336, 69, 3},
	{224, 336, 60, 3}};

#define SIDECAR_LOGO_DISC_X	370	// the solid disc, x10
#define SIDECAR_LOGO_DISC_Y	270
#define SIDECAR_LOGO_DISC_R	75

#endif
