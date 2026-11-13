// test_display_hint.cpp - pi/ui/display_hint.h and the boot screen's hint (sidecar_splash_hint) on the Mac: the Pi 5's
// 640x480 fallback without an EDID is told apart from a screen that works, and the hint fits a 640x480 boot screen in
// both looks, large, under the progress bar, which then steps without touching it.
#include "../../ui/display_hint.h"
#include "../../ui/sidecar_splash.h"
#include "check.h"
#include <cstdint>
#include <cstring>
#include <vector>

static void decision (void)
{
	CHECK (sidecar_no_edid_fallback (640, 480, 0));		// the report: 640x480, the EDID's header 00 00 00
	CHECK (!sidecar_no_edid_fallback (640, 480, 1));		// a screen whose EDID prefers 640x480: legal, no hint
	CHECK (!sidecar_no_edid_fallback (1920, 1080, 0));	// forced hotplug without an EDID: the firmware's 1080p
	CHECK (!sidecar_no_edid_fallback (800, 480, 0));
	CHECK (!sidecar_no_edid_fallback (1024, 600, 0));
	CHECK (!sidecar_no_edid_fallback (480, 640, 0));
	CHECK (!sidecar_no_edid_fallback (0, 0, 0));		// no frame buffer
	CHECK (strchr (SIDECAR_NOEDID_STATUS, '"') == 0 && strchr (SIDECAR_NOEDID_STATUS, '\\') == 0);	// JSON as it is
}

// rows [y0, y1) differ between a and b
static bool rows_differ (const std::vector<uint16_t> &a, const std::vector<uint16_t> &b, unsigned w, unsigned y0, unsigned y1)
{
	return memcmp (&a[y0 * w], &b[y0 * w], (y1 - y0) * w * sizeof (uint16_t)) != 0;
}

static void screen (int style, unsigned W, unsigned H, int min_scale)
{
	sidecar_splash_style (style);
	std::vector<uint16_t> fb (W * H), base, hinted, full;
	sidecar_splash_draw (fb.data (), W, W, H);
	base = fb;
	sidecar_splash_progress (fb.data (), W, W, H, 8, 8);		// the bar's whole extent
	full = fb;
	fb = base;
	int s = sidecar_splash_hint (fb.data (), W, W, H, SIDECAR_NOEDID_TEXT);
	CHECK (s >= min_scale);
	hinted = fb;
	// the hint drew, and only below the last row the full bar changes
	unsigned barEnd = 0, hintTop = H;
	for (unsigned y = 0; y < H; y++)
	{
		if (rows_differ (base, full, W, y, y + 1)) barEnd = y + 1;
		if (hintTop == H && rows_differ (base, hinted, W, y, y + 1)) hintTop = y;
	}
	CHECK (barEnd > 0 && hintTop < H);
	CHECK (hintTop > barEnd);
	// white text, centred: its leftmost and rightmost columns as far from the edges (within a scale step)
	unsigned left = W, right = 0;
	for (unsigned y = hintTop; y < H; y++)
		for (unsigned x = 0; x < W; x++)
			if (hinted[y * W + x] != base[y * W + x])
			{
				CHECK (hinted[y * W + x] == 0xFFFF);
				if (x < left) left = x;
				if (x > right) right = x;
			}
	CHECK (left >= 16 && right < W - 16);
	CHECK ((int) left - (int) (W - 1 - right) <= s && (int) (W - 1 - right) - (int) left <= s);
	// the bar's steps leave the hint alone
	for (unsigned k = 1; k <= 8; k++) sidecar_splash_progress (fb.data (), W, W, H, k, 8);
	CHECK (!rows_differ (fb, hinted, W, hintTop, H));
}

int main ()
{
	decision ();
	screen (0, 640, 480, 3);		// OLED (the default look): at least 3x, the 5x9 face 27 pixels tall
	screen (1, 640, 480, 3);		// SIDECAR
	screen (0, 800, 480, 3);
	screen (1, 800, 480, 3);
	screen (0, 1280, 720, 3);
	screen (1, 1024, 600, 3);
	printf ("test_display_hint: OK\n");
	return 0;
}
