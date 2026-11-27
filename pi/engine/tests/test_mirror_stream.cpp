// pi/engine/tests/test_mirror_stream.cpp - mirror_stream.h (usb=mirror's audio for a second port, docs/teensy-mirror.md):
// make build/test_mirror_stream
//
// A made-up configuration in the A4's shape (8 channels in, 6 out at alt 3; pid.codes IDs, no box's bytes).
#include "mirror_stream.h"
#include "check.h"

#include <cstdio>
#include <vector>

static std::vector<uint8_t> Cfg;
static void D (std::initializer_list<int> b) { for (int x : b) Cfg.push_back ((uint8_t) x); }

static void BuildFixture ()
{
	Cfg.clear ();
	D ({9, 2, 0, 0, 3, 1, 0, 0xC0, 0});
	D ({9, 4, 0, 0, 0, 0xFF, 0, 0, 0});
	const int In[4] = {0, 32 + 7 * 4 * 2, 32 + 7 * 4 * 4, 32 + 7 * 4 * 8};
	const int Out[4] = {0, 32 + 7 * 4 * 2, 32 + 7 * 4 * 4, 32 + 7 * 4 * 6};
	D ({9, 4, 1, 0, 0, 0xFF, 0, 0, 0});
	for (int a = 1; a <= 3; a++) { D ({9, 4, 1, a, 1, 0xFF, 0, 0, 0}); D ({7, 5, 0x83, 3, In[a] & 0xFF, In[a] >> 8, 1}); }
	D ({9, 4, 2, 0, 0, 0xFF, 0, 0, 0});
	for (int a = 1; a <= 3; a++) { D ({9, 4, 2, a, 1, 0xFF, 0, 0, 0}); D ({7, 5, 0x03, 3, Out[a] & 0xFF, Out[a] >> 8, 1}); }
	Cfg[2] = Cfg.size () & 0xFF;
	Cfg[3] = Cfg.size () >> 8;
}

static void Block (uint8_t *b, int ch, uint16_t counter, int32_t base, int extra_at = -1)
{
	memset (b, 0, ob::block_bytes (ch));
	ob::write_header (b, counter);
	if (extra_at >= 0) b[4 + extra_at] = 0x5A;
	for (int f = 0; f < ob::kFramesPerBlock; f++)
		for (int c = 0; c < ch; c++) ob::put_be32 (b + ob::kHeaderBytes + 4 * (ch * f + c), (base + f * 16 + c) << 8);
}

int main ()
{
	using namespace mirror;
	BuildFixture ();
	Layout l;
	CHECK (parse (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &l));
	static Stream s;
	s.init (8);

	// nothing chosen yet: IN dropped, OUT bad
	uint8_t In12[ob::block_bytes (12)], Out[ob::block_bytes (12)];
	Block (In12, 12, 0, 1000);
	CHECK (s.in_block (In12, Out) == 0);
	CHECK (!s.set_alt (l, Cfg.data (), (unsigned) Cfg.size (), 1, 4));
	CHECK (s.set_alt (l, Cfg.data (), (unsigned) Cfg.size (), 1, 3) && s.in_ch == 8 && s.out_ch == 0);
	CHECK (s.set_alt (l, Cfg.data (), (unsigned) Cfg.size (), 2, 3) && s.out_ch == 6);

	// IN: header kept, the first 8 of 12 channels per frame
	CHECK (s.in_block (In12, Out) == (unsigned) ob::block_bytes (8));
	CHECK (memcmp (Out, In12, ob::kHeaderBytes) == 0);
	for (int f = 0; f < ob::kFramesPerBlock; f++)
		for (int c = 0; c < 8; c++) CHECK (ob::block_sample (Out, 8, f, c) == ob::block_sample (In12, 12, f, c));

	// OUT: a wrong size is bad; a 6-channel block lands in the ring; the box hears it only after the priming target
	uint8_t B6[ob::block_bytes (6)];
	Block (B6, 6, 0, 0);
	s.out_block (B6, 10, 0);
	CHECK (s.st.out_bad == 1);
	int32_t F[ob::kFramesPerBlock][12];
	uint16_t comp = 0, pi = 700;
	for (int k = 0; k < 6; k++)		// 42 frames: below the target of 48
	{
		Block (B6, 6, comp, k * 1000);
		s.out_block (B6, sizeof B6, pi);
		comp += 7;
	}
	memset (F, 0, sizeof F);
	s.to_box (F, 7);
	CHECK (F[0][0] == 0 && s.priming);
	for (int k = 6; k < 8; k++) { Block (B6, 6, comp, k * 1000); s.out_block (B6, sizeof B6, pi); comp += 7; }
	memset (F, 0, sizeof F);
	s.to_box (F, 7);
	CHECK (!s.priming);
	CHECK (F[0][0] == (0 << 8) && F[1][0] == (16 << 8) && F[0][5] == (5 << 8) && F[0][6] == 0);	// <<8 back to 32-bit

	// per-block bytes: due margin (8) blocks past the Pi's counter at the first block
	s.init (8);
	CHECK (s.set_alt (l, Cfg.data (), (unsigned) Cfg.size (), 2, 3));
	Block (B6, 6, 100, 0, 3);
	s.out_block (B6, sizeof B6, 500);
	CHECK (s.st.out_extra == 1);
	uint8_t Pi[ob::block_bytes (6)];
	memset (Pi, 0, sizeof Pi);
	CHECK (!s.out_extra (Pi, (uint16_t) (500 + 7 * 7)));
	CHECK (s.out_extra (Pi, (uint16_t) (500 + 8 * 7)) && Pi[4 + 3] == 0x5A && Pi[1] == 3);
	// the computer fell behind the Pi (its block due in one already sent): re-anchored, counted
	Block (B6, 6, 107, 0, 1);
	s.out_block (B6, sizeof B6, 600);
	CHECK (s.st.anchor == 1);

	printf ("test_mirror_stream: ok\n");
	return 0;
}
