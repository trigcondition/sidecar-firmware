// pi/ui/dsi_rotate.h - a portrait DSI panel (the Raspberry Pi Touch Display 2: 720x1280) mounted landscape: the frame
// (1280x720, what the display code works in) to the panel's own pixels and back (docs/pi-display.md "Touch Display 2").
// The angle is Linux's: how far the picture is turned counter-clockwise on the panel. 270 is a quarter turn clockwise,
// the frame's top edge along the panel's right edge; 90 the other way. No Circle here: pi/engine/tests/test_dsi_rotate.cpp
// runs it on the Mac.
#ifndef DSI_ROTATE_H
#define DSI_ROTATE_H

#include <stdint.h>

namespace dsirot {

// frame pixel (fx, fy) -> panel pixel (px, py); nRot 90 or 270, nPW / nPH the panel's size
inline void to_panel (unsigned nRot, unsigned nPW, unsigned nPH, unsigned fx, unsigned fy, unsigned *px, unsigned *py)
{
	*px = nRot == 270 ? nPW - 1 - fy : fy;
	*py = nRot == 270 ? fx : nPH - 1 - fx;
}

// panel pixel -> frame pixel, the inverse: the touch controller reports panel pixels
inline void to_frame (unsigned nRot, unsigned nPW, unsigned nPH, unsigned px, unsigned py, unsigned *fx, unsigned *fy)
{
	*fx = nRot == 270 ? py : nPH - 1 - py;
	*fy = nRot == 270 ? nPW - 1 - px : px;
}

// The w x h frame area at (fx, fy) from pSrc (rows nStride pixels apart) into pFb, the panel's frame buffer (rows nPitch
// pixels apart). One panel row per frame column, so the writes are contiguous; the reads step through at most a 48-row
// band (LVGL's draw buffer), which stays in the cache. *pRow / *pCol / *pRows / *pCols: the panel rectangle written (for
// the cache clean). false: the area is not inside the frame, nothing written. Directly into the frame buffer: a copy
// through CDisplay::SetArea would cost a barrier for each of a band's 1024 panel rows.
inline bool blit (uint16_t *pFb, unsigned nPitch, unsigned nPW, unsigned nPH, unsigned nRot, unsigned fx, unsigned fy,
		  unsigned w, unsigned h, const uint16_t *pSrc, unsigned nStride,
		  unsigned *pRow, unsigned *pCol, unsigned *pRows, unsigned *pCols)
{
	if (w == 0 || h == 0 || fx >= nPH || fy >= nPW || w > nPH - fx || h > nPW - fy) return false;
	if (nRot != 90 && nRot != 270) return false;
	const unsigned nCol = nRot == 270 ? nPW - fy - h : fy;		// the panel rectangle's first column
	if (nRot == 270)
	{
		for (unsigned x = 0; x < w; x++)			// frame column x: panel row fx + x, right to left
		{
			uint16_t *d = pFb + (fx + x) * nPitch + nCol + h - 1;
			const uint16_t *s = pSrc + x;
			for (unsigned y = 0; y < h; y++, s += nStride) *d-- = *s;
		}
		*pRow = fx;
	}
	else
	{
		const unsigned nTop = nPH - fx - w;
		for (unsigned x = 0; x < w; x++)			// frame column x: panel row nPH - 1 - fx - x, left to right
		{
			uint16_t *d = pFb + (nPH - 1 - fx - x) * nPitch + nCol;
			const uint16_t *s = pSrc + x;
			for (unsigned y = 0; y < h; y++, s += nStride) *d++ = *s;
		}
		*pRow = nTop;
	}
	*pCol = nCol;
	*pRows = w;
	*pCols = h;
	return true;
}

}	// namespace dsirot

#endif
