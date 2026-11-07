// pi/engine/dhcpd_lite.h - the USB-C network's DHCP server (docs/usb-network.md): one computer on a point-to-point
// cable, one address to lease. DISCOVER -> OFFER, REQUEST for our address -> ACK (any other address, or a REQUEST to
// another server's id, -> NAK / ignored), INFORM -> ACK without a lease; RELEASE and DECLINE need no answer.
// The lease carries a netmask and nothing else: no router (3) and no DNS (6), so the computer never sends its own
// traffic through the Sidecar and keeps its internet on Wi-Fi or its own cable. Portable, header only
// (tests/test_dhcpd_lite.cpp). The Pi side (usbnet_pi.cpp) sends every answer to 255.255.255.255:68.
#ifndef _dhcpd_lite_h
#define _dhcpd_lite_h

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace dhcpd {

constexpr unsigned kServerPort = 67, kClientPort = 68;
constexpr size_t kFixed = 236;				// the BOOTP header before the magic cookie
constexpr size_t kMinReply = 300;			// BOOTP's minimum message (some clients drop shorter ones)
constexpr size_t kMaxReply = 576;

enum Type : uint8_t { kDiscover = 1, kOffer = 2, kRequest = 3, kDecline = 4, kAck = 5, kNak = 6, kRelease = 7, kInform = 8 };

struct Config
{
	uint8_t server[4];		// the Pi's address on the cable
	uint8_t client[4];		// the one address leased
	uint8_t mask[4];
	uint32_t lease_s;		// e.g. 3600
};

struct Request
{
	uint8_t type = 0;		// 0: no option 53 (not DHCP)
	bool has_requested = false, has_server = false;
	uint8_t requested[4] = {0, 0, 0, 0};	// option 50
	uint8_t server[4] = {0, 0, 0, 0};	// option 54
};

// The options of a client's message (n bytes from its start). false: not a BOOTREQUEST / Ethernet / DHCP message
inline bool parse (const uint8_t *p, size_t n, Request *pReq)
{
	*pReq = Request ();
	if (n < kFixed + 4 || p[0] != 1 || p[1] != 1 || p[2] != 6) return false;
	if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) return false;
	size_t i = kFixed + 4;
	while (i < n)
	{
		uint8_t code = p[i++];
		if (code == 0) continue;			// pad
		if (code == 255) break;				// end
		if (i >= n) return false;
		uint8_t len = p[i++];
		if (i + len > n) return false;
		if (code == 53 && len == 1) pReq->type = p[i];
		else if (code == 50 && len == 4) { memcpy (pReq->requested, p + i, 4); pReq->has_requested = true; }
		else if (code == 54 && len == 4) { memcpy (pReq->server, p + i, 4); pReq->has_server = true; }
		i += len;
	}
	return pReq->type != 0;
}

// The answer to a client's message in pOut (at least kMaxReply bytes): its length, 0 for none
inline size_t reply (const Config &c, const uint8_t *p, size_t n, uint8_t *pOut, size_t nCap)
{
	Request r;
	if (nCap < kMaxReply || !parse (p, n, &r)) return 0;
	uint8_t type = 0;
	bool bLease = false;
	switch (r.type)
	{
	case kDiscover:
		type = kOffer;
		bLease = true;
		break;
	case kRequest: {
		if (r.has_server && memcmp (r.server, c.server, 4) != 0) return 0;	// it chose another server
		const uint8_t *want = r.has_requested ? r.requested : p + 12;		// SELECTING / INIT-REBOOT, else ciaddr
		if (memcmp (want, c.client, 4) == 0) { type = kAck; bLease = true; }
		else type = kNak;
		} break;
	case kInform:
		type = kAck;
		break;
	default:
		return 0;					// DECLINE, RELEASE, ours, unknown
	}
	memset (pOut, 0, kMaxReply);
	pOut[0] = 2;						// BOOTREPLY
	pOut[1] = 1;
	pOut[2] = 6;
	memcpy (pOut + 4, p + 4, 4);				// xid
	memcpy (pOut + 10, p + 10, 2);				// flags (the broadcast bit)
	if (r.type == kInform) memcpy (pOut + 12, p + 12, 4);	// ciaddr back
	if (bLease) memcpy (pOut + 16, c.client, 4);		// yiaddr
	if (type != kNak) memcpy (pOut + 20, c.server, 4);	// siaddr
	memcpy (pOut + 24, p + 24, 4);				// giaddr
	memcpy (pOut + 28, p + 28, 16);				// chaddr
	size_t i = kFixed;
	const uint8_t magic[4] = {99, 130, 83, 99};
	memcpy (pOut + i, magic, 4);
	i += 4;
	auto opt = [&] (uint8_t code, const uint8_t *v, uint8_t len) { pOut[i++] = code; pOut[i++] = len; memcpy (pOut + i, v, len); i += len; };
	auto u32 = [] (uint32_t v, uint8_t *b) { b[0] = (uint8_t) (v >> 24); b[1] = (uint8_t) (v >> 16); b[2] = (uint8_t) (v >> 8); b[3] = (uint8_t) v; };
	opt (53, &type, 1);
	opt (54, c.server, 4);
	if (type != kNak)
	{
		uint8_t b[4];
		if (bLease)
		{
			u32 (c.lease_s, b); opt (51, b, 4);
			u32 (c.lease_s / 2, b); opt (58, b, 4);
			u32 (c.lease_s / 8 * 7, b); opt (59, b, 4);
		}
		opt (1, c.mask, 4);
		// no 3 (router), no 6 (DNS): the computer's own internet stays where it is
	}
	pOut[i++] = 255;
	return i < kMinReply ? kMinReply : i;
}

}	// namespace dhcpd

#endif
