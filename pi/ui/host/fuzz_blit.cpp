#include "../dsi_rotate.h"
extern "C" int fz_blit (uint16_t *fb, unsigned fx, unsigned fy, unsigned w, unsigned h, const uint16_t *src, unsigned stride)
{
	unsigned r, c, rs, cs;
	return dsirot::blit (fb, 720, 720, 1280, 270, fx, fy, w, h, src, stride, &r, &c, &rs, &cs);
}
