// pi/engine/tests/test_teensy_hex.cpp - teensy_hex.h (the Teensy 4.1's program for its bootloader, docs/teensy-flash.md):
// make build/test_teensy_hex
// TEENSY_HEX=<file.hex> also parses a real build (teensy/mirror_relay/build.ps1's) and prints its blocks, for a byte
// comparison against another writer's report stream (TEENSY_HEX_DUMP=<out.bin>: every report in send order, boot last).
#include "teensy_hex.h"
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace teensyhex;

static std::vector<uint8_t> Image (kCodeSize);

static std::string Rec (unsigned type, unsigned addr, const std::vector<uint8_t> &d, bool bBadSum = false)
{
	std::vector<uint8_t> r = {(uint8_t) d.size (), (uint8_t) (addr >> 8), (uint8_t) addr, (uint8_t) type};
	r.insert (r.end (), d.begin (), d.end ());
	uint8_t s = 0;
	for (uint8_t b : r) s = (uint8_t) (s + b);
	r.push_back ((uint8_t) (0x100 - s + (bBadSum ? 1 : 0)));
	std::string out = ":";
	char h[3];
	for (uint8_t b : r) { snprintf (h, sizeof h, "%02X", b); out += h; }
	return out + "\r\n";
}
static std::string Linear (uint32_t hi) { return Rec (4, 0, {(uint8_t) (hi >> 8), (uint8_t) hi}); }
static std::string End (void) { return Rec (1, 0, {}); }

static bool Parse (const std::string &s, Parser &p, unsigned nPiece = 0)
{
	p.begin (Image.data ());
	if (nPiece == 0) { if (!p.feed (s.data (), (unsigned) s.size ())) return false; }
	else for (size_t i = 0; i < s.size (); i += nPiece)
		if (!p.feed (s.data () + i, (unsigned) std::min ((size_t) nPiece, s.size () - i))) return false;
	return p.finish ();
}

int main (void)
{
	static Parser p;	// (1 KB of block bits: not on the stack)

	// ---- a small program: 0x60000000 (block 0), a record across blocks 1/2, block 5 only 0xFF, block 9 data
	std::string h = Linear (0x6000);
	std::vector<uint8_t> d16;
	for (int i = 0; i < 16; i++) d16.push_back ((uint8_t) (i + 1));
	h += Rec (0, 0x0000, d16);
	h += Rec (0, 0x07F8, d16);		// 0x7F8..0x807: blocks 1 and 2
	h += Rec (0, 0x1400, std::vector<uint8_t> (16, 0xFF));	// block 5: data, but all 0xFF -> not sent
	h += Rec (3, 0, {0, 0, 0, 0});		// a start address: ignored
	h += Rec (0, 0x2400, {0xAA});		// block 9
	h += End ();
	CHECK (Parse (h, p));
	CHECK (p.bytes () == 16 + 16 + 16 + 1);
	CHECK (p.low () == 0 && p.high () == 0x2401);
	CHECK (p.image ()[0] == 1 && p.image ()[15] == 16 && p.image ()[16] == 0xFF);
	CHECK (p.image ()[0x7F8] == 1 && p.image ()[0x807] == 16);
	CHECK (p.sends (0) && p.sends (1) && p.sends (2) && !p.sends (3) && !p.sends (5) && p.sends (9) && !p.sends (10));
	CHECK (p.next (0) == 0 && p.next (3) == 9 && p.next (10) == kBlocks);
	CHECK (p.count () == 4);

	// the same in every piece size (an upload's chunks cut lines anywhere, \r\n included)
	for (unsigned k = 1; k <= 50; k++)
	{
		static Parser q;
		CHECK (Parse (h, q, k));
		CHECK (q.bytes () == p.bytes () && q.count () == 4);
	}

	// ---- the reports
	CHECK (Parse (h, p));
	std::vector<uint8_t> r (kReport, 0x55);
	block_report (p.image (), 9, r.data ());
	CHECK (r[0] == 0x00 && r[1] == 0x24 && r[2] == 0x00);	// 9 * 1024 = 0x2400
	for (unsigned i = 3; i < kHeader; i++) CHECK (r[i] == 0);
	CHECK (r[kHeader] == 0xAA && r[kHeader + 1] == 0xFF && r[kReport - 1] == 0xFF);
	block_report (p.image (), 1, r.data ());
	CHECK (r[0] == 0x00 && r[1] == 0x04 && r[2] == 0x00 && r[kHeader + 0x3F8] == 1 && r[kHeader + 0x3FF] == 8);
	boot_report (r.data ());
	CHECK (r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF);
	for (unsigned i = 3; i < kReport; i++) CHECK (r[i] == 0);
	CHECK (kReport == 1088 && kBlocks == 7936);
	// the top block's offset: 3 bytes
	block_report (p.image (), kBlocks - 1, r.data ());
	CHECK (r[0] == 0x00 && r[1] == 0xFC && r[2] == 0x7B);	// 0x7BFC00

	// block 0 sent even when the program does not start there
	{
		std::string g = Linear (0x6000) + Rec (0, 0x0C00, {7}) + End ();
		CHECK (Parse (g, p));
		CHECK (p.sends (0) && p.next (1) == 3 && p.count () == 2);
	}
	// segment addresses (type 02) reach the flash too only through a linear base; 02 with base 0 is outside it
	{
		std::string g = Rec (2, 0, {0x10, 0x00}) + Rec (0, 0, {1}) + End ();
		CHECK (!Parse (g, p));
	}

	// ---- refusals: nothing that is not a whole, clean program
	{
		CHECK (!Parse (Linear (0x6000) + Rec (0, 0, {1, 2}), p));
		CHECK (strstr (p.err (), "end record") != 0);
		CHECK (!Parse (Linear (0x6000) + Rec (0, 0, {1, 2}, true) + End (), p));
		CHECK (strstr (p.err (), "checksum") != 0 && p.errline () == 2);
		CHECK (!Parse (Rec (0, 0, {1}) + End (), p));	// no linear base: 0x00000000 is not the flash
		CHECK (strstr (p.err (), "outside") != 0);
		CHECK (!Parse (Linear (0x607B) + Rec (0, 0xFFF8, d16) + End (), p));	// runs past 0x607C0000
		CHECK (!Parse (Linear (0x6000) + Rec (0, 0, {1}) + End () + Rec (0, 0, {2}), p));
		CHECK (strstr (p.err (), "after the end") != 0);
		CHECK (!Parse (End (), p));
		CHECK (strstr (p.err (), "no data") != 0);
		CHECK (!Parse (Linear (0x6000) + ":0100000001FG\r\n" + End (), p));
		CHECK (!Parse (Linear (0x6000) + ":02000000010\r\n" + End (), p));
		CHECK (!Parse (Linear (0x6000) + "01000000FF\r\n" + End (), p));
		CHECK (!Parse (Linear (0x6000) + ":0300000001FB\r\n" + End (), p));	// length byte 3, one data byte
		CHECK (!Parse (Linear (0x6000) + Rec (6, 0, {}) + End (), p));
		CHECK (!Parse (std::string (2000, 'A'), p));
		CHECK (strstr (p.err (), "too long") != 0);
		// a blank line and trailing spaces are fine
		CHECK (Parse (Linear (0x6000) + "\r\n" + Rec (0, 0, {1}) + "  \n" + End (), p));
	}

	// ---- the relay's build from its log
	{
		char b[24];
		const char *L = "usb2: up\nmirror relay up, protocol 1.00, build deb98be+\nconnect: 1935:0011\n";
		CHECK (find_build (L, (unsigned) strlen (L), b, sizeof b) && strcmp (b, "deb98be+") == 0);
		const char *L2 = "mirror relay up, protocol 1.00, build aaaaaaa\nx\nmirror relay up, protocol 1.00, build bbbbbbb";
		CHECK (find_build (L2, (unsigned) strlen (L2), b, sizeof b) && strcmp (b, "bbbbbbb") == 0);
		const char *L3 = "connect refused\n";
		CHECK (!find_build (L3, (unsigned) strlen (L3), b, sizeof b));
		CHECK (retry_ms (0) == 45000 && retry_ms (3) == 45000 && retry_ms (4) == 2000);
	}

	// ---- a real build, when named
	if (const char *pFile = getenv ("TEENSY_HEX"))
	{
		FILE *f = fopen (pFile, "rb");
		CHECK (f != 0);
		std::string s;
		char buf[4096];
		size_t n;
		while ((n = fread (buf, 1, sizeof buf, f)) > 0) s.append (buf, n);
		fclose (f);
		CHECK (Parse (s, p, 48 * 1024));
		printf ("%s: %u data bytes, offsets 0x%x..0x%x, %u blocks to send:", pFile, p.bytes (), p.low (), p.high (), p.count ());
		for (uint32_t b = p.next (0); b < kBlocks; b = p.next (b + 1)) printf (" %u", b);
		printf ("\n");
		if (const char *pDump = getenv ("TEENSY_HEX_DUMP"))
		{
			FILE *o = fopen (pDump, "wb");
			CHECK (o != 0);
			for (uint32_t b = p.next (0); b < kBlocks; b = p.next (b + 1)) { block_report (p.image (), b, r.data ()); fwrite (r.data (), 1, kReport, o); }
			boot_report (r.data ());
			fwrite (r.data (), 1, kReport, o);
			fclose (o);
		}
	}

	printf ("test_teensy_hex: PASS\n");
	return 0;
}
