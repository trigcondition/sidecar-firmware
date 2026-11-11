// pi/engine/tests/test_sidecar_desc.cpp - sidecar_desc.h: the Sidecar device's configuration descriptor. Without the
// USB-C network the bytes are exactly 0.4.2's (sidecar_gadget.cpp's BuildConfig, captured from it before the move, both
// channel builds); with it, the whole composite device is walked: lengths, interface numbers and alternates, the
// interface associations, every endpoint address once per direction, and the NCM function's class descriptors.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "sidecar_desc.h"

using namespace sidecar_desc;

static const unsigned char kGolden12[318] = {
	9, 2, 62, 1, 5, 1, 0, 128, 250, 8, 11, 0, 3, 1, 0, 32, 4, 9, 4, 0, 0, 0, 1, 1,
	32, 0, 9, 36, 1, 0, 2, 8, 75, 0, 0, 8, 36, 10, 16, 1, 5, 0, 0, 17, 36, 2, 1, 1,
	1, 0, 16, 12, 0, 0, 0, 0, 16, 0, 0, 0, 12, 36, 3, 2, 1, 3, 0, 1, 16, 0, 0, 0,
	17, 36, 2, 3, 1, 2, 0, 16, 12, 0, 0, 0, 0, 32, 0, 0, 0, 12, 36, 3, 4, 1, 1, 0,
	3, 16, 0, 0, 0, 9, 4, 1, 0, 0, 1, 2, 32, 0, 9, 4, 1, 1, 2, 1, 2, 32, 0, 16,
	36, 1, 1, 0, 1, 1, 0, 0, 0, 12, 0, 0, 0, 0, 16, 6, 36, 2, 1, 3, 24, 7, 5, 1,
	5, 212, 1, 2, 8, 37, 1, 0, 0, 0, 0, 0, 7, 5, 130, 17, 4, 0, 4, 9, 4, 2, 0, 0,
	1, 2, 32, 0, 9, 4, 2, 1, 1, 1, 2, 32, 0, 16, 36, 1, 4, 0, 1, 1, 0, 0, 0, 12,
	0, 0, 0, 0, 32, 6, 36, 2, 1, 3, 24, 7, 5, 131, 5, 212, 1, 2, 8, 37, 1, 0, 0, 0,
	0, 0, 8, 11, 3, 2, 1, 3, 0, 5, 9, 4, 3, 0, 0, 1, 1, 0, 0, 9, 36, 1, 0, 1,
	9, 0, 1, 4, 9, 4, 4, 0, 2, 1, 3, 0, 0, 7, 36, 1, 0, 1, 65, 0, 6, 36, 2, 1,
	1, 5, 6, 36, 2, 2, 2, 0, 9, 36, 3, 1, 3, 1, 2, 1, 5, 9, 36, 3, 2, 4, 1, 1,
	1, 0, 9, 5, 4, 2, 0, 2, 0, 0, 0, 5, 37, 1, 1, 1, 9, 5, 133, 2, 0, 2, 0, 0,
	0, 5, 37, 1, 1, 3
};
static const unsigned char kGolden20[318] = {
	9, 2, 62, 1, 5, 1, 0, 128, 250, 8, 11, 0, 3, 1, 0, 32, 4, 9, 4, 0, 0, 0, 1, 1,
	32, 0, 9, 36, 1, 0, 2, 8, 75, 0, 0, 8, 36, 10, 16, 1, 5, 0, 0, 17, 36, 2, 1, 1,
	1, 0, 16, 12, 0, 0, 0, 0, 16, 0, 0, 0, 12, 36, 3, 2, 1, 3, 0, 1, 16, 0, 0, 0,
	17, 36, 2, 3, 1, 2, 0, 16, 20, 0, 0, 0, 0, 32, 0, 0, 0, 12, 36, 3, 4, 1, 1, 0,
	3, 16, 0, 0, 0, 9, 4, 1, 0, 0, 1, 2, 32, 0, 9, 4, 1, 1, 2, 1, 2, 32, 0, 16,
	36, 1, 1, 0, 1, 1, 0, 0, 0, 12, 0, 0, 0, 0, 16, 6, 36, 2, 1, 3, 24, 7, 5, 1,
	5, 212, 1, 2, 8, 37, 1, 0, 0, 0, 0, 0, 7, 5, 130, 17, 4, 0, 4, 9, 4, 2, 0, 0,
	1, 2, 32, 0, 9, 4, 2, 1, 1, 1, 2, 32, 0, 16, 36, 1, 4, 0, 1, 1, 0, 0, 0, 20,
	0, 0, 0, 0, 32, 6, 36, 2, 1, 3, 24, 7, 5, 131, 5, 12, 3, 2, 8, 37, 1, 0, 0, 0,
	0, 0, 8, 11, 3, 2, 1, 3, 0, 5, 9, 4, 3, 0, 0, 1, 1, 0, 0, 9, 36, 1, 0, 1,
	9, 0, 1, 4, 9, 4, 4, 0, 2, 1, 3, 0, 0, 7, 36, 1, 0, 1, 65, 0, 6, 36, 2, 1,
	1, 5, 6, 36, 2, 2, 2, 0, 9, 36, 3, 1, 3, 1, 2, 1, 5, 9, 36, 3, 2, 4, 1, 1,
	1, 0, 9, 5, 4, 2, 0, 2, 0, 0, 0, 5, 37, 1, 1, 1, 9, 5, 133, 2, 0, 2, 0, 0,
	0, 5, 37, 1, 1, 3
};

struct Walk { unsigned ifaces, alts, eps, iads; bool ep_in[16], ep_out[16]; unsigned iface_alts[16]; bool ok; };

// every descriptor's length inside the block; the interfaces (number, alternate) and endpoints counted
static Walk walk (const unsigned char *p, unsigned n)
{
	Walk w;
	memset (&w, 0, sizeof w);
	w.ok = true;
	unsigned i = 0, cur = 99;
	while (i < n)
	{
		unsigned len = p[i];
		if (len < 2 || i + len > n) { w.ok = false; break; }
		unsigned type = p[i + 1];
		if (type == 4)
		{
			CHECK (len == 9);
			cur = p[i + 2];
			CHECK (cur < 16);
			if (p[i + 3] == 0) w.ifaces++;
			else w.alts++;
			w.iface_alts[cur]++;
		}
		else if (type == 5)
		{
			CHECK (len == 7 || len == 9);
			unsigned a = p[i + 2] & 15;
			bool in = p[i + 2] & 0x80;
			CHECK (a != 0 && cur != 99);
			// an address appears once per direction across the device; alternates of one interface may repeat one,
			// this device has none that do
			CHECK (!(in ? w.ep_in[a] : w.ep_out[a]));
			(in ? w.ep_in[a] : w.ep_out[a]) = true;
			w.eps++;
		}
		else if (type == 11) w.iads++;
		i += len;
	}
	return w;
}

int main ()
{
	unsigned char b[kCap];
	Offsets o;
	// 0.4.2's bytes, 12 in (MaxIn 468) and 20 in (780)
	unsigned n = build (b, sizeof b, 12, 12, 13 * 12 * 3, 13 * 12 * 3, false, &o);
	CHECK (n == sizeof kGolden12 && memcmp (b, kGolden12, n) == 0);
	CHECK (o.out == 141 && o.fb == 156 && o.in == 203 && o.midi_out == 290 && o.midi_in == 304);
	CHECK (o.net.notify == 0 && o.net.in == 0 && o.net.out == 0);
	n = build (b, sizeof b, 20, 12, 13 * 20 * 3, 13 * 12 * 3, false, &o);
	CHECK (n == sizeof kGolden20 && memcmp (b, kGolden20, n) == 0);
	CHECK (build (b, kCap - 1, 12, 12, 468, 468, true, &o) == 0);	// too small a buffer: refused, nothing written past it

	// with the network: the same 318 bytes first (only bNumInterfaces and wTotalLength differ), then 85 more
	static const unsigned kIns[] = {12u, 20u};
	for (unsigned nIn : kIns)
	{
		unsigned char a[kCap];
		Offsets oa;
		unsigned na = build (a, sizeof a, nIn, 12, 13 * nIn * 3, 468, false, &oa);
		n = build (b, sizeof b, nIn, 12, 13 * nIn * 3, 468, true, &o);
		CHECK (n == na + usbnet::kDescLen && n <= kCap);
		CHECK (b[2] == (n & 0xFF) && b[3] == (n >> 8) && b[4] == 7 && a[4] == 5);
		CHECK (memcmp (a + 5, b + 5, na - 5) == 0);
		CHECK (o.out == oa.out && o.midi_in == oa.midi_in);

		Walk w = walk (b + 9, n - 9);
		CHECK (w.ok);
		CHECK (w.ifaces == 7);					// 0..6, each with an alternate 0
		for (unsigned k = 0; k < 7; k++) CHECK (w.iface_alts[k] >= 1);
		CHECK (w.iface_alts[1] == 2 && w.iface_alts[2] == 2 && w.iface_alts[6] == 2 && w.iface_alts[5] == 1);
		CHECK (w.iads == 3);					// audio, MIDI, network
		CHECK (w.eps == 8);
		CHECK (w.ep_out[1] && w.ep_in[2] && w.ep_in[3] && w.ep_out[4] && w.ep_in[5]);
		CHECK (w.ep_in[6] && w.ep_in[7] && w.ep_out[7]);	// the network's
		CHECK (!w.ep_out[6] && !w.ep_out[2] && !w.ep_out[3]);

		// the network's endpoints where the offsets say, and its class descriptors
		CHECK (b[o.net.notify] == 7 && b[o.net.notify + 2] == 0x86 && b[o.net.notify + 3] == 0x03 && b[o.net.notify + 4] == 16);
		CHECK (b[o.net.in] == 7 && b[o.net.in + 2] == 0x87 && b[o.net.in + 3] == 0x02 && b[o.net.in + 4] == 0 && b[o.net.in + 5] == 2);
		CHECK (b[o.net.out] == 7 && b[o.net.out + 2] == 0x07 && b[o.net.out + 3] == 0x02);
		const unsigned char *f = b + na;
		CHECK (f[0] == 8 && f[1] == 11 && f[2] == 5 && f[3] == 2 && f[4] == 2 && f[5] == 0x0D && f[7] == StrNet);
		CHECK (f[8] == 9 && f[9] == 4 && f[10] == 5 && f[13] == 2 && f[14] == 0x0D);
		CHECK (f[17] == 5 && f[18] == 0x24 && f[19] == 0x00);			// header
		CHECK (f[22] == 5 && f[24] == 0x06 && f[25] == 5 && f[26] == 6);	// union: 5 controls 6
		CHECK (f[27] == 13 && f[29] == 0x0F && f[30] == StrNetMac && f[35] == 0xEA && f[36] == 0x05);	// 1514
		CHECK (f[40] == 6 && f[42] == 0x1A && f[43] == 0x00 && f[44] == 0x01);	// NCM 1.00
		// the IADs cover what they claim: audio 0..2, MIDI 3..4, network 5..6
		CHECK (b[9] == 8 && b[11] == 0 && b[12] == 3);
	}
	printf ("test_sidecar_desc: OK\n");
	return 0;
}
