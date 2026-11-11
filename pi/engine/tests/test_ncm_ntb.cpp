// pi/engine/tests/test_ncm_ntb.cpp - ncm_ntb.h: the CDC-NCM framing both ways, the EP0 class requests and the
// notifications. Built blocks parse back to the same frames; hostile OUT blocks (short, wrong signatures, offsets past
// the block, NDP loops, too many datagrams, frames too short or long) never hand on a byte from outside the block.
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "ncm_ntb.h"

using namespace ncm;

static std::vector<std::vector<uint8_t>> parsed;
static void collect (const uint8_t *p, unsigned n) { parsed.emplace_back (p, p + n); }

static std::vector<uint8_t> frame (unsigned n, uint8_t seed)
{
	std::vector<uint8_t> f (n);
	for (unsigned i = 0; i < n; i++) f[i] = (uint8_t) (seed + i * 7);
	return f;
}

// a hand-made OUT block: NTH16, datagrams at the given offsets, one NDP at nNdp
static std::vector<uint8_t> block (size_t nSize, unsigned nNdp, const std::vector<std::pair<unsigned, unsigned>> &e, unsigned nNdpLen = 0)
{
	std::vector<uint8_t> b (nSize, 0xEE);
	wr32 (&b[0], kNth16Sig);
	wr16 (&b[4], 12);
	wr16 (&b[6], 1);
	wr16 (&b[8], (uint16_t) nSize);
	wr16 (&b[10], (uint16_t) nNdp);
	if (nNdpLen == 0) nNdpLen = 8 + 4 * (unsigned) (e.size () + 1);
	wr32 (&b[nNdp], kNdp16Sig);
	wr16 (&b[nNdp + 4], (uint16_t) nNdpLen);
	wr16 (&b[nNdp + 6], 0);
	for (size_t i = 0; i < e.size (); i++) { wr16 (&b[nNdp + 8 + 4 * i], (uint16_t) e[i].first); wr16 (&b[nNdp + 10 + 4 * i], (uint16_t) e[i].second); }
	if (nNdp + 8 + 4 * e.size () + 4 <= nSize) { wr16 (&b[nNdp + 8 + 4 * e.size ()], 0); wr16 (&b[nNdp + 10 + 4 * e.size ()], 0); }
	return b;
}

int main ()
{
	// ---- builder -> parser round trip, frame sizes from minimum to maximum
	{
		std::vector<uint8_t> buf (kNtbInMax);
		Ntb16Builder nb;
		nb.begin (buf.data (), buf.size (), 7);
		std::vector<std::vector<uint8_t>> in;
		const unsigned sizes[] = {14, 15, 60, 61, 62, 63, 1514, 590, 1000, 333};
		for (unsigned s : sizes) { in.push_back (frame (s, (uint8_t) s)); CHECK (nb.add (in.back ().data (), s)); }
		CHECK (nb.count () == 10);
		size_t n = nb.finish ();
		CHECK (n > 0 && n <= buf.size () && n % 512 != 0 && n % 4 == 0);
		CHECK (rd16 (&buf[6]) == 7 && rd16 (&buf[8]) == n);
		parsed.clear ();
		ParseStats st;
		CHECK (parse16 (buf.data (), n, 16, collect, &st) == kOk && st.datagrams == 10 && st.bad == 0);
		CHECK (parsed == in);
		// every datagram 4-byte aligned (wNdpInDivisor 4, remainder 0) and the NDP too
		unsigned ndp = rd16 (&buf[10]);
		CHECK (ndp % 4 == 0);
		for (unsigned i = 0; i < 10; i++) CHECK (rd16 (&buf[ndp + 8 + 4 * i]) % 4 == 0);
	}
	// not frames: refused, the block unchanged
	{
		uint8_t buf[2048];
		Ntb16Builder nb;
		nb.begin (buf, sizeof buf, 0);
		std::vector<uint8_t> f = frame (1515, 1);
		CHECK (!nb.add (f.data (), 13) && !nb.add (f.data (), 1515) && nb.count () == 0 && nb.finish () == 0);
		// fills up: what does not fit is refused, the rest stays a valid block within the cap
		unsigned k = 0;
		while (nb.add (f.data (), 600)) k++;
		CHECK (k == 3);
		size_t n = nb.finish ();
		CHECK (n <= sizeof buf);
		parsed.clear ();
		CHECK (parse16 (buf, n, 16, collect) == kOk && parsed.size () == 3);
	}
	// at most kMaxPerBlock frames
	{
		std::vector<uint8_t> buf (kNtbInMax);
		Ntb16Builder nb;
		nb.begin (buf.data (), buf.size (), 0);
		std::vector<uint8_t> f = frame (60, 3);
		unsigned k = 0;
		while (nb.add (f.data (), 60)) k++;
		CHECK (k == Ntb16Builder::kMaxPerBlock);
	}
	// the end of a transfer: a block whose length is a multiple of 512 gets 4 more bytes (else the computer waits for
	// more), unless it fills the computer's whole NTB input size; every length from 1 to 3 frames tried
	{
		unsigned nPadded = 0, nExact = 0;
		for (unsigned cap : {2048u, 2050u, 4096u, 16384u})
			for (unsigned a = 14; a <= 1514; a++)
			{
				std::vector<uint8_t> buf (cap);
				Ntb16Builder nb;
				nb.begin (buf.data (), cap, 0);
				std::vector<uint8_t> f = frame (a, 9);
				if (!nb.add (f.data (), a)) continue;
				size_t n = nb.finish ();
				CHECK (n <= (cap & ~3u));
				if (n % 512 == 0) { CHECK (n == (cap & ~3u)); nExact++; }
				if (rd16 (&buf[8]) != n) CHECK (false);
				parsed.clear ();
				CHECK (parse16 (buf.data (), n, 16, collect) == kOk && parsed.size () == 1 && parsed[0] == f);
				if ((n - 4) % 512 == 0 && n > 4) nPadded++;
			}
		CHECK (nPadded > 0);		// the rule was exercised
		(void) nExact;
	}

	// ---- hostile OUT blocks
	std::vector<uint8_t> f60 = frame (60, 5);
	{
		// a good one first
		std::vector<uint8_t> b = block (200, 100, {{12, 60}});
		memcpy (&b[12], f60.data (), 60);
		parsed.clear ();
		CHECK (parse16 (b.data (), b.size (), 8, collect) == kOk && parsed.size () == 1 && parsed[0] == f60);
		// the transfer shorter than the block says
		CHECK (parse16 (b.data (), 199, 8, collect) == kBadLength);
		CHECK (parse16 (b.data (), 11, 8, collect) == kShort);
		// signatures
		std::vector<uint8_t> c = b; c[0] ^= 1;
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNth);
		c = b; wr16 (&c[4], 16);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNth);
		c = b; c[100] ^= 1;
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		c = b; wr32 (&c[100], 0x314D434E);		// NCM1 (CRC): not supported
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		// the NDP pointer: unaligned, inside the header, past the block
		c = b; wr16 (&c[10], 102);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		c = b; wr16 (&c[10], 8);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		c = b; wr16 (&c[10], 196);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		// NDP length too small, unaligned, past the block
		c = b; wr16 (&c[104], 12);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		c = b; wr16 (&c[104], 18);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		c = b; wr16 (&c[104], 104);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
		// a loop (next NDP back to itself) and a chain going backwards
		c = b; wr16 (&c[106], 100);
		parsed.clear ();
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp && parsed.size () == 1);
		c = b; wr16 (&c[106], 12);
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kBadNdp);
	}
	{
		// datagrams outside the block, inside the header, too short, too long: skipped and counted, the good ones kept
		std::vector<uint8_t> b = block (1800, 1700, {{12, 60}, {150, 60}, {4, 60}, {1690, 60}, {200, 13}, {200, 1515}, {300, 60}});
		memcpy (&b[12], f60.data (), 60);
		memcpy (&b[300], f60.data (), 60);
		parsed.clear ();
		ParseStats st;
		CHECK (parse16 (b.data (), b.size (), 8, collect, &st) == kOk);
		CHECK (st.datagrams == 4 && st.bad == 3 && parsed.size () == 4);		// 12, 150, 1690 (inside: the NDP's bytes), 300
		CHECK (parsed[0] == f60 && parsed[3] == f60);
		// a datagram ending exactly at the block's end is in; one byte more is out
		std::vector<uint8_t> e = block (512, 16, {{448, 64}});
		parsed.clear ();
		CHECK (parse16 (e.data (), e.size (), 8, collect, &st) == kOk && st.datagrams == 1);
		e = block (512, 16, {{449, 64}});
		CHECK (parse16 (e.data (), e.size (), 8, collect, &st) == kOk && st.datagrams == 0 && st.bad == 1);
	}
	{
		// more datagrams than the device takes: the first nMax handed on, then refused
		std::vector<std::pair<unsigned, unsigned>> e;
		for (unsigned i = 0; i < 10; i++) e.push_back ({12u + 64 * i, 60u});
		std::vector<uint8_t> b = block (1000, 900, e);
		parsed.clear ();
		CHECK (parse16 (b.data (), b.size (), 8, collect) == kTooMany && parsed.size () == 8);
		// an NDP without its 0/0 end: the entries its length allows, no more
		std::vector<uint8_t> c = block (300, 200, {{12, 60}, {80, 60}}, 16);	// room for 2 entries, no end
		parsed.clear ();
		CHECK (parse16 (c.data (), c.size (), 8, collect) == kOk && parsed.size () == 2);
	}
	{
		// two NDPs chained forward: both read
		std::vector<uint8_t> b = block (400, 200, {{12, 60}});
		wr16 (&b[206], 300);
		wr32 (&b[300], kNdp16Sig); wr16 (&b[304], 16); wr16 (&b[306], 0);
		wr16 (&b[308], 80); wr16 (&b[310], 60); wr16 (&b[312], 0); wr16 (&b[314], 0);
		parsed.clear ();
		CHECK (parse16 (b.data (), b.size (), 8, collect) == kOk && parsed.size () == 2);
	}
	// random garbage never reads outside the buffer (ASan) and never hands on a datagram outside the block
	{
		unsigned seed = 1;
		for (int k = 0; k < 20000; k++)
		{
			size_t n = 12 + (seed % 600);
			std::vector<uint8_t> b (n);
			for (auto &x : b) { seed = seed * 1103515245 + 12345; x = (uint8_t) (seed >> 16); }
			wr32 (&b[0], kNth16Sig);
			wr16 (&b[4], 12);
			if (k & 1) wr16 (&b[8], (uint16_t) n);
			if (k & 2 && n > 40) { unsigned ndp = (unsigned) ((n / 2) & ~3u); wr16 (&b[10], (uint16_t) ndp); if (ndp + 8 <= n) wr32 (&b[ndp], kNdp16Sig); }
			const uint8_t *lo = b.data (), *hi = b.data () + n;
			parse16 (b.data (), n, 8, [&] (const uint8_t *p, unsigned len) { CHECK (p >= lo + 12 && p + len <= hi && len >= 14 && len <= 1514); });
		}
	}

	// ---- EP0
	{
		Control c;
		c.reset ();
		uint8_t in[64], out[8];
		CHECK (c.request (0xA1, kGetNtbParameters, 0, 28, 0, in) == 28);
		CHECK (rd16 (in) == 28 && rd16 (in + 2) == 1 && rd32 (in + 4) == kNtbInMax && rd16 (in + 8) == 4 && rd16 (in + 12) == 4);
		CHECK (rd32 (in + 16) == kNtbOutMax && rd16 (in + 20) == 4 && rd16 (in + 24) == 4 && rd16 (in + 26) == kOutMaxDatagrams);
		CHECK (c.request (0xA1, kGetNtbParameters, 0, 8, 0, in) == 8);		// cut to wLength
		CHECK (c.request (0x21, kGetNtbParameters, 0, 28, 0, in) == -1);	// wrong direction
		CHECK (c.request (0xA1, kGetNtbFormat, 0, 2, 0, in) == 2 && rd16 (in) == 0);
		CHECK (c.request (0x21, kSetNtbFormat, 0, 0, 0, in) == 0);
		CHECK (c.request (0x21, kSetNtbFormat, 1, 0, 0, in) == -1);		// NTB32
		CHECK (c.request (0xA1, kGetNtbInputSize, 0, 4, 0, in) == 4 && rd32 (in) == kNtbInMax);
		wr32 (out, 4096);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 4, out, in) == 0 && c.ntb_in_size () == 4096);
		wr32 (out, 8192); wr16 (out + 4, 0); wr16 (out + 6, 0);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 8, out, in) == 0 && c.ntb_in_size () == 8192);
		wr32 (out, 1024);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 4, out, in) == -1 && c.ntb_in_size () == 8192);	// below the floor
		wr32 (out, kNtbInMax + 4);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 4, out, in) == -1 && c.ntb_in_size () == 8192);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 2, out, in) == -1);
		CHECK (c.request (0x21, kSetNtbInputSize, 0, 4, 0, in) == -1);
		c.reset ();
		CHECK (c.ntb_in_size () == kNtbInMax);
		CHECK (c.request (0x21, kSetPacketFilter, 0x000F, 0, 0, in) == 0 && c.packet_filter () == 0x000F);
		CHECK (c.request (0x21, kSetCrcMode, 0, 0, 0, in) == 0 && c.request (0x21, kSetCrcMode, 1, 0, 0, in) == -1);
		CHECK (c.request (0xA1, kGetCrcMode, 0, 2, 0, in) == 2 && rd16 (in) == 0);
		CHECK (c.request (0xA1, kGetMaxDatagram, 0, 2, 0, in) == 2 && rd16 (in) == 1514);
		wr16 (out, 1514);
		CHECK (c.request (0x21, kSetMaxDatagram, 0, 2, out, in) == 0);
		wr16 (out, 1400);
		CHECK (c.request (0x21, kSetMaxDatagram, 0, 2, out, in) == -1);
		CHECK (c.request (0x21, kSetMulticastFilters, 0, 0, 0, in) == 0);
		CHECK (c.request (0x21, kSendEncapsulated, 0, 0, 0, in) == -1);
		CHECK (c.request (0xA1, kGetEncapsulated, 0, 8, 0, in) == -1);
		CHECK (c.request (0xA1, kGetNetAddress, 0, 6, 0, in) == -1);
		CHECK (c.request (0xA1, 0x55, 0, 6, 0, in) == -1);
	}
	// ---- notifications
	{
		uint8_t n[16];
		CHECK (notify_speed (n, 5, 480000000u) == 16);
		const uint8_t s[8] = {0xA1, 0x2A, 0, 0, 5, 0, 8, 0};
		CHECK (memcmp (n, s, 8) == 0 && rd32 (n + 8) == 480000000u && rd32 (n + 12) == 480000000u);
		CHECK (notify_connection (n, 5, true) == 8);
		const uint8_t u[8] = {0xA1, 0x00, 1, 0, 5, 0, 0, 0};
		CHECK (memcmp (n, u, 8) == 0);
		notify_connection (n, 5, false);
		CHECK (n[2] == 0);
	}
	printf ("test_ncm_ntb: OK\n");
	return 0;
}
