// test_dsi_rotate.cpp - pi/ui/dsi_rotate.h on the Mac: the frame's pixels on a portrait panel at 90 and 270, the UI's
// 1024x600 centred in 1280x720, every pixel of a blit against the mapping, and a touch at a pixel coming back to that
// pixel. Run with an argument "bench": the blit's cost on this machine, one full 1024x600 screen in LVGL's 48-row bands.
#include "../../ui/dsi_rotate.h"
#include "check.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static const unsigned PW = 720, PH = 1280, FW = 1280, FH = 720;		// the panel, the landscape frame
static const unsigned UW = 1024, UH = 600, OX = (FW - UW) / 2, OY = (FH - UH) / 2;

static void corners (void)
{
	unsigned x, y;
	// 270: a quarter turn clockwise. The frame's top left is the panel's top right, its top right the bottom right.
	// Measured on the owner's mounting (a photo of the unrotated UI): memory (0,0) at the viewed bottom left, columns up,
	// rows to the right, so this is the right way up there
	dsirot::to_panel (270, PW, PH, 0, 0, &x, &y); CHECK (x == 719 && y == 0);
	dsirot::to_panel (270, PW, PH, FW - 1, 0, &x, &y); CHECK (x == 719 && y == 1279);
	dsirot::to_panel (270, PW, PH, 0, FH - 1, &x, &y); CHECK (x == 0 && y == 0);
	dsirot::to_panel (270, PW, PH, FW - 1, FH - 1, &x, &y); CHECK (x == 0 && y == 1279);
	// 90: the other way. The frame's top left is the panel's bottom left
	dsirot::to_panel (90, PW, PH, 0, 0, &x, &y); CHECK (x == 0 && y == 1279);
	dsirot::to_panel (90, PW, PH, FW - 1, 0, &x, &y); CHECK (x == 0 && y == 0);
	dsirot::to_panel (90, PW, PH, 0, FH - 1, &x, &y); CHECK (x == 719 && y == 1279);
	dsirot::to_panel (90, PW, PH, FW - 1, FH - 1, &x, &y); CHECK (x == 719 && y == 0);
	CHECK (OX == 128 && OY == 60);
	// the UI's top left: 128, 60 in the frame
	dsirot::to_panel (270, PW, PH, OX, OY, &x, &y); CHECK (x == 659 && y == 128);
	dsirot::to_panel (90, PW, PH, OX, OY, &x, &y); CHECK (x == 60 && y == 1151);
}

static void roundtrip (unsigned nRot)
{
	for (unsigned fy = 0; fy < FH; fy++)
		for (unsigned fx = 0; fx < FW; fx++)
		{
			unsigned px, py, x, y;
			dsirot::to_panel (nRot, PW, PH, fx, fy, &px, &py);
			CHECK (px < PW && py < PH);
			dsirot::to_frame (nRot, PW, PH, px, py, &x, &y);	// a touch on that panel pixel
			CHECK (x == fx && y == fy);
		}
}

// a finger on a UI pixel: panel pixel -> frame -> the centred UI's coordinates, as DsiTouchHandler does it
static void touch (unsigned nRot)
{
	const unsigned Points[][2] = {{0, 0}, {UW - 1, 0}, {0, UH - 1}, {UW - 1, UH - 1}, {512, 300}, {100, 450}};
	for (auto &q : Points)
	{
		unsigned px, py, x, y;
		dsirot::to_panel (nRot, PW, PH, OX + q[0], OY + q[1], &px, &py);
		dsirot::to_frame (nRot, PW, PH, px, py, &x, &y);
		CHECK (x - OX == q[0] && y - OY == q[1]);
	}
}

static uint16_t Tag (unsigned x, unsigned y) { return (uint16_t) (x * 31 + y * 7 + 1); }

static void blit_area (unsigned nRot, unsigned fx, unsigned fy, unsigned w, unsigned h, unsigned nPitch)
{
	std::vector<uint16_t> Fb (nPitch * PH, 0), Src (w * h);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++) Src[y * w + x] = Tag (fx + x, fy + y);
	unsigned nRow, nCol, nRows, nCols;
	CHECK (dsirot::blit (Fb.data (), nPitch, PW, PH, nRot, fx, fy, w, h, Src.data (), w, &nRow, &nCol, &nRows, &nCols));
	CHECK (nRows == w && nCols == h && nRow + nRows <= PH && nCol + nCols <= PW);
	std::vector<uint16_t> Want (nPitch * PH, 0);
	for (unsigned y = 0; y < h; y++)
		for (unsigned x = 0; x < w; x++)
		{
			unsigned px, py;
			dsirot::to_panel (nRot, PW, PH, fx + x, fy + y, &px, &py);
			CHECK (py >= nRow && py < nRow + nRows && px >= nCol && px < nCol + nCols);	// the cache clean covers it
			Want[py * nPitch + px] = Src[y * w + x];
		}
	CHECK (memcmp (Fb.data (), Want.data (), Fb.size () * sizeof (uint16_t)) == 0);	// those pixels, and nothing else
}

static void blits (unsigned nRot)
{
	blit_area (nRot, 0, 0, FW, FH, PW);			// the whole frame
	blit_area (nRot, OX, OY, UW, 48, PW);			// a band at the UI's place
	blit_area (nRot, OX + 300, OY + 552, 37, 48, PW);	// a dirty area at the UI's bottom
	blit_area (nRot, FW - 1, FH - 1, 1, 1, PW);		// the last pixel
	blit_area (nRot, 0, 0, 1, FH, PW);			// one column, one row
	blit_area (nRot, 0, FH - 1, FW, 1, PW);
	blit_area (nRot, 5, 6, 40, 30, PW + 13);		// a frame buffer with longer rows
	std::vector<uint16_t> Fb (PW * PH), Src (2 * FW);
	unsigned a, b, c, d;
	CHECK (!dsirot::blit (Fb.data (), PW, PW, PH, nRot, 0, 0, FW + 1, 1, Src.data (), FW, &a, &b, &c, &d));
	CHECK (!dsirot::blit (Fb.data (), PW, PW, PH, nRot, 0, 0, 1, FH + 1, Src.data (), 1, &a, &b, &c, &d));
	CHECK (!dsirot::blit (Fb.data (), PW, PW, PH, nRot, FW, 0, 1, 1, Src.data (), 1, &a, &b, &c, &d));
	CHECK (!dsirot::blit (Fb.data (), PW, PW, PH, nRot, 0, FH, 1, 1, Src.data (), 1, &a, &b, &c, &d));
	CHECK (!dsirot::blit (Fb.data (), PW, PW, PH, nRot, 0, 0, 0, 1, Src.data (), 1, &a, &b, &c, &d));
}

static void bench (void)
{
	std::vector<uint16_t> Fb (PW * PH), Band (UW * 48);
	for (unsigned i = 0; i < Band.size (); i++) Band[i] = (uint16_t) i;
	unsigned a, b, c, d;
	for (unsigned nRot : {270u, 90u})
	{
		double dBest = 1e9;
		for (int nRun = 0; nRun < 50; nRun++)
		{
			auto t0 = std::chrono::steady_clock::now ();
			for (unsigned y = 0; y < UH; y += 48)
			{
				unsigned h = UH - y < 48 ? UH - y : 48;
				dsirot::blit (Fb.data (), PW, PW, PH, nRot, OX, OY + y, UW, h, Band.data (), UW, &a, &b, &c, &d);
			}
			double dUs = std::chrono::duration<double, std::micro> (std::chrono::steady_clock::now () - t0).count ();
			if (dUs < dBest) dBest = dUs;
		}
		printf ("rotation %u: 1024x600 in 48-row bands: %.0f us (best of 50), %.2f ns a pixel, no cache clean\n", nRot, dBest,
			dBest * 1000 / (UW * UH));
	}
}

int main (int argc, char **)
{
	if (argc > 1) { bench (); return 0; }
	corners ();
	for (unsigned nRot : {270u, 90u})
	{
		roundtrip (nRot);
		touch (nRot);
		blits (nRot);
	}
	unsigned a = 0, b = 0, c = 0, d = 0;
	uint16_t Fb[4] = {0}, Src[1] = {7};
	CHECK (!dsirot::blit (Fb, PW, PW, PH, 180, 0, 0, 1, 1, Src, 1, &a, &b, &c, &d));	// only a quarter turn here
	printf ("test_dsi_rotate: ok\n");
	return 0;
}
