// pi/ui/host/splash.c - the boot screen (sidecar_splash.c) on the Mac: 800x480 (or WxH), 3 of 8 stages, as a PPM.
// splash OUT [STYLE [WxH]] (STYLE 0 OLED, 1 CLASSIC).
#include "../sidecar_splash.h"
#include <stdio.h>
static int W = 800, H = 480;
static uint16_t s_Fb[1920 * 1080];
#include <stdlib.h>
int main (int argc, char **argv)
{
	if (argc > 3 && (sscanf (argv[3], "%dx%d", &W, &H) != 2 || W < 320 || H < 240 || W * H > 1920 * 1080)) return 2;
	sidecar_splash_style (argc > 2 ? atoi (argv[2]) : 0);
	sidecar_splash_draw (s_Fb, W, W, H);
	sidecar_splash_progress (s_Fb, W, W, H, 3, 8);
	FILE *f = fopen (argc > 1 ? argv[1] : "splash.ppm", "wb");
	fprintf (f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++)
	{
		unsigned c = s_Fb[i];
		unsigned char px[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3)};
		fwrite (px, 1, 3, f);
	}
	fclose (f);
	return 0;
}
