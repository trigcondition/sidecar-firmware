// pi/engine/tests/test_dhcpd_lite.cpp - dhcpd_lite.h: the USB-C network's DHCP server. A client's whole exchange
// (DISCOVER, REQUEST, renew, INFORM), the refusals (another address, another server, malformed messages), and that no
// answer ever offers a router or DNS server.
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "dhcpd_lite.h"

using namespace dhcpd;

static const Config kCfg = {{172, 31, 77, 1}, {172, 31, 77, 2}, {255, 255, 255, 0}, 3600};
static const uint8_t kMac[6] = {0x06, 0x11, 0x22, 0x33, 0x44, 0x55};

static std::vector<uint8_t> msg (uint8_t type, const uint8_t *req = 0, const uint8_t *server = 0, const uint8_t *ciaddr = 0,
				 bool bBroadcast = false)
{
	std::vector<uint8_t> m (300, 0);
	m[0] = 1; m[1] = 1; m[2] = 6;
	m[4] = 0xDE; m[5] = 0xAD; m[6] = 0xBE; m[7] = 0xEF;
	if (bBroadcast) m[10] = 0x80;
	if (ciaddr) memcpy (&m[12], ciaddr, 4);
	memcpy (&m[28], kMac, 6);
	m[236] = 99; m[237] = 130; m[238] = 83; m[239] = 99;
	size_t i = 240;
	m[i++] = 53; m[i++] = 1; m[i++] = type;
	if (req) { m[i++] = 50; m[i++] = 4; memcpy (&m[i], req, 4); i += 4; }
	if (server) { m[i++] = 54; m[i++] = 4; memcpy (&m[i], server, 4); i += 4; }
	m[i++] = 0; m[i++] = 0;				// pads
	m[i++] = 12; m[i++] = 3; m[i++] = 'm'; m[i++] = 'a'; m[i++] = 'c';	// a host name (ignored)
	m[i++] = 255;
	return m;
}

struct Answer { uint8_t type; bool router, dns; uint8_t yiaddr[4], mask[4]; uint32_t lease; bool has_mask, has_server; };

static Answer read (const uint8_t *p, size_t n)
{
	Answer a;
	memset (&a, 0, sizeof a);
	CHECK (n >= kMinReply && n <= kMaxReply);
	CHECK (p[0] == 2 && p[1] == 1 && p[2] == 6);
	CHECK (p[4] == 0xDE && p[7] == 0xEF && memcmp (p + 28, kMac, 6) == 0);
	CHECK (p[236] == 99 && p[237] == 130 && p[238] == 83 && p[239] == 99);
	memcpy (a.yiaddr, p + 16, 4);
	size_t i = 240;
	while (i < n && p[i] != 255)
	{
		uint8_t c = p[i], l = p[i + 1];
		CHECK (i + 2 + l <= n);
		if (c == 53) a.type = p[i + 2];
		if (c == 3) a.router = true;
		if (c == 6) a.dns = true;
		if (c == 1) { memcpy (a.mask, p + i + 2, 4); a.has_mask = true; }
		if (c == 54) { CHECK (memcmp (p + i + 2, kCfg.server, 4) == 0); a.has_server = true; }
		if (c == 51) a.lease = (uint32_t) p[i + 2] << 24 | p[i + 3] << 16 | p[i + 4] << 8 | p[i + 5];
		i += 2 + l;
	}
	CHECK (i < n);					// ended
	CHECK (!a.router && !a.dns);			// never: the computer's internet stays its own
	CHECK (a.has_server);
	return a;
}

int main ()
{
	uint8_t out[kMaxReply];
	// DISCOVER -> OFFER of .2, the mask, the lease, the broadcast flag kept
	std::vector<uint8_t> m = msg (kDiscover, 0, 0, 0, true);
	size_t n = reply (kCfg, m.data (), m.size (), out, sizeof out);
	Answer a = read (out, n);
	CHECK (a.type == kOffer && memcmp (a.yiaddr, kCfg.client, 4) == 0 && a.lease == 3600 && a.has_mask);
	CHECK (memcmp (a.mask, kCfg.mask, 4) == 0 && out[10] == 0x80);
	// REQUEST (selecting: requested address + our server id) -> ACK
	m = msg (kRequest, kCfg.client, kCfg.server);
	a = read (out, reply (kCfg, m.data (), m.size (), out, sizeof out));
	CHECK (a.type == kAck && memcmp (a.yiaddr, kCfg.client, 4) == 0 && a.lease == 3600);
	// renewing: no option 50, ciaddr = the address -> ACK
	m = msg (kRequest, 0, 0, kCfg.client);
	a = read (out, reply (kCfg, m.data (), m.size (), out, sizeof out));
	CHECK (a.type == kAck);
	// INIT-REBOOT for an address from another network (a laptop that was elsewhere) -> NAK, no lease, no address
	const uint8_t other[4] = {192, 168, 1, 50};
	m = msg (kRequest, other);
	a = read (out, reply (kCfg, m.data (), m.size (), out, sizeof out));
	CHECK (a.type == kNak && a.lease == 0 && !a.has_mask && a.yiaddr[0] == 0);
	// REQUEST naming another server: not ours to answer
	m = msg (kRequest, kCfg.client, other);
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	// INFORM: ACK with the mask, no lease, ciaddr back
	m = msg (kInform, 0, 0, kCfg.client);
	n = reply (kCfg, m.data (), m.size (), out, sizeof out);
	a = read (out, n);
	CHECK (a.type == kAck && a.lease == 0 && a.has_mask && memcmp (out + 12, kCfg.client, 4) == 0 && a.yiaddr[0] == 0);
	// RELEASE, DECLINE, a server's own message: no answer
	m = msg (kRelease);
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kDecline);
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kOffer);
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	// malformed: BOOTREPLY, not Ethernet, no magic, too short, no option 53, an option running past the end
	m = msg (kDiscover); m[0] = 2;
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kDiscover); m[1] = 6;
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kDiscover); m[237] = 0;
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kDiscover);
	CHECK (reply (kCfg, m.data (), 239, out, sizeof out) == 0);
	m = msg (kDiscover); m[240] = 0; m[241] = 0; m[242] = 0;	// 53 gone (pads)
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	m = msg (kDiscover); m.resize (243); m[241] = 40;		// option 53 claims 40 bytes past the end
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) == 0);
	// too small an answer buffer: none
	m = msg (kDiscover);
	CHECK (reply (kCfg, m.data (), m.size (), out, kMaxReply - 1) == 0);
	// no end option and only the cookie: parsed to the buffer's end without reading past it (ASan)
	m = msg (kDiscover); m.resize (243);
	CHECK (reply (kCfg, m.data (), m.size (), out, sizeof out) > 0);
	// random messages never read outside the buffer and every answer is well-formed
	unsigned seed = 7;
	for (int k = 0; k < 20000; k++)
	{
		size_t len = 236 + (seed % 120);
		std::vector<uint8_t> r (len);
		for (auto &x : r) { seed = seed * 1103515245 + 12345; x = (uint8_t) (seed >> 16); }
		r[0] = 1; r[1] = 1; r[2] = 6;
		if (len >= 240) { r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99; }
		if (len >= 243 && (k & 1)) { r[240] = 53; r[241] = 1; r[242] = (uint8_t) (1 + k % 8); }
		memcpy (&r[4], m.data () + 4, 4);
		memcpy (&r[28], kMac, 6);
		size_t got = reply (kCfg, r.data (), r.size (), out, sizeof out);
		if (got) read (out, got);
	}
	printf ("test_dhcpd_lite: OK\n");
	return 0;
}
