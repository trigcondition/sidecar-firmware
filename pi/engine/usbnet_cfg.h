// pi/engine/usbnet_cfg.h - the USB-C network (docs/usb-network.md): its cmdline.txt keys, the boot's choice between it
// and Wi-Fi/LAN, who is trusted, its MAC addresses and its descriptor block. Portable, header only
// (tests/test_usbnet_cfg.cpp); the Pi side is usbnet_pi.{h,cpp}, the gadget's half sidecar_gadget.cpp.
//
// Circle runs one network stack on one device (CNetSubSystem is a singleton). So the network is chosen once, at boot:
//   net=auto (default)  a computer on USB-C whose USB network driver starts within usbwait= seconds -> the USB-C
//                       network (Wi-Fi not joined this boot: its chip stays up for scans, so Wi-Fi can be set up from
//                       the page); no computer on the bus within usbquiet= ms, or none that starts the network -> as
//                       before (Wi-Fi with a stored network, else Ethernet). A Wi-Fi join being tried (wifi.try) wins.
//   net=usb             the USB-C network whatever is connected (a computer plugged in later gets it)
//   net=wifi            never the USB-C network: Wi-Fi as before (Ethernet without a stored network)
//   net=lan             Ethernet, Wi-Fi not joined (its chip for scans as wifi= says)
//   usbnet=on|off       the network function in the USB-C device (CLASS COMPLIANT only; default on). Off: the device is
//                       exactly the one before (audio + MIDI, bcdDevice 3.00)
//   usbip=A.B.C.D       the Pi's address on the cable (default 172.31.77.1, a /24); the computer is leased .2 (.1 when
//                       the Pi is .2)
//   usbwait=S           how long a computer on the bus may take to start its USB network (default 5 s, 1..30)
//   usbquiet=MS         no USB traffic (start-of-frame) this long after the gadget started: no computer (default 700)
//   usbtrust=1|0        a connection over the cable needs no pairing (default 1; 0: pair as on Wi-Fi/LAN)
#ifndef _usbnet_cfg_h
#define _usbnet_cfg_h

#include <stdint.h>
#include <string.h>

namespace usbnet {

enum Net { kAuto, kUsb, kWifi, kLan };

inline const char *net_name (Net n)	{ return n == kUsb ? "usb" : n == kWifi ? "wifi" : n == kLan ? "lan" : "auto"; }

// net=: "auto" "usb" "wifi" "lan" (also "ethernet" / "eth"); false: none of them (*pNet unchanged)
inline bool parse_net (const char *p, Net *pNet)
{
	if (p == 0) return false;
	if (!strcmp (p, "auto")) *pNet = kAuto;
	else if (!strcmp (p, "usb")) *pNet = kUsb;
	else if (!strcmp (p, "wifi")) *pNet = kWifi;
	else if (!strcmp (p, "lan") || !strcmp (p, "ethernet") || !strcmp (p, "eth")) *pNet = kLan;
	else return false;
	return true;
}

// on/off words: "1" "on" "0" "off"; anything else (or none): bDefault
inline bool parse_onoff (const char *p, bool bDefault)
{
	if (p == 0) return bDefault;
	if (!strcmp (p, "1") || !strcmp (p, "on")) return true;
	if (!strcmp (p, "0") || !strcmp (p, "off")) return false;
	return bDefault;
}

// usbip=: a dotted quad usable as the Pi's address on a /24: not 0.x, 127.x, multicast or above, host part 1..254
inline bool parse_ip (const char *p, uint8_t ip[4])
{
	if (p == 0) return false;
	uint8_t v[4];
	for (int i = 0; i < 4; i++)
	{
		if (*p < '0' || *p > '9') return false;
		unsigned n = 0, d = 0;
		while (*p >= '0' && *p <= '9') { n = n * 10 + (unsigned) (*p++ - '0'); if (++d > 3 || n > 255) return false; }
		v[i] = (uint8_t) n;
		if (i < 3 && *p++ != '.') return false;
	}
	if (*p != 0 || v[0] == 0 || v[0] == 127 || v[0] >= 224 || v[3] == 0 || v[3] == 255) return false;
	memcpy (ip, v, 4);
	return true;
}

constexpr uint8_t kDefaultIp[4] = {172, 31, 77, 1};
constexpr uint8_t kMask[4] = {255, 255, 255, 0};
constexpr unsigned kDefaultWaitS = 5, kDefaultQuietMs = 700;

// the computer's lease: the Pi's /24, host .2 (.1 when the Pi is .2)
inline void client_ip (const uint8_t pi[4], uint8_t out[4])
{
	memcpy (out, pi, 3);
	out[3] = pi[3] == 2 ? 1 : 2;
}

struct Options
{
	Net net = kAuto;
	bool usbnet = true, trust = true;
	uint8_t ip[4] = {172, 31, 77, 1};
	unsigned wait_s = kDefaultWaitS, quiet_ms = kDefaultQuietMs;
	bool bad_net = false, bad_ip = false;	// a value given and not understood (logged; the default used)
};

inline unsigned clamp_num (const char *p, unsigned nDefault, unsigned nMin, unsigned nMax)
{
	if (p == 0 || *p == 0) return nDefault;
	unsigned n = 0;
	for (; *p; p++) { if (*p < '0' || *p > '9' || n > 100000) return nDefault; n = n * 10 + (unsigned) (*p - '0'); }
	return n < nMin ? nMin : n > nMax ? nMax : n;
}

// cmdline.txt's values (0: not there)
inline Options options (const char *pNet, const char *pUsbnet, const char *pIp, const char *pWait, const char *pQuiet, const char *pTrust)
{
	Options o;
	if (pNet && !parse_net (pNet, &o.net)) o.bad_net = true;
	o.usbnet = parse_onoff (pUsbnet, true);
	if (pIp && !parse_ip (pIp, o.ip)) { o.bad_ip = true; memcpy (o.ip, kDefaultIp, 4); }
	o.wait_s = clamp_num (pWait, kDefaultWaitS, 1, 30);
	o.quiet_ms = clamp_num (pQuiet, kDefaultQuietMs, 100, 5000);
	o.trust = parse_onoff (pTrust, true);
	return o;
}

// ---- the boot's choice ---------------------------------------------------------------------------------------------

enum Plan { kToday, kProbe, kForced };

struct Inputs
{
	Net net = kAuto;
	bool function = false;		// the network function is in the USB-C device now (usbnet on, CLASS COMPLIANT, gadget up)
	bool wifi_trial = false;	// a Wi-Fi join is being tried this boot (wifi.try "trying")
	bool link_tcp = false;		// link=tcp: the engine's link to the emulator runs over Ethernet
};

// *ppWhy: the reason for the log line
inline Plan plan (const Inputs &in, const char **ppWhy)
{
	if (in.net == kWifi) { *ppWhy = "net=wifi"; return kToday; }
	if (in.net == kLan) { *ppWhy = "net=lan"; return kToday; }
	if (!in.function) { *ppWhy = "no USB-C network function (usbnet=off, OVERBRIDGE mode or no gadget)"; return kToday; }
	if (in.net == kUsb) { *ppWhy = "net=usb"; return kForced; }
	if (in.wifi_trial) { *ppWhy = "a Wi-Fi join is being tried this boot"; return kToday; }
	if (in.link_tcp) { *ppWhy = "link=tcp (the emulator link needs Ethernet)"; return kToday; }
	*ppWhy = "net=auto";
	return kProbe;
}

// net=auto's wait at boot, a step every few ms: the time since the gadget started, whether the bus has carried a
// start-of-frame since then (a computer drives it), whether the computer's USB network driver has started (it selected
// the data interface). Decided once: the USB-C network, no computer, or a computer without a USB network driver.
// The bus is watched kMinWatchMs at least before either "no" (the Wi-Fi chip's start often takes longer than usbquiet:
// one sample during a bus reset, with no start-of-frame for a few ms, must not decide).
constexpr unsigned kMinWatchMs = 100;
enum Probe { kWaiting, kUsbUp, kNoHost, kNoDriver };

class BootProbe
{
public:
	BootProbe (unsigned nQuietMs, unsigned nWaitS) : m_nQuiet (nQuietMs), m_nWait (nWaitS * 1000u) {}
	Probe step (unsigned nMs, bool bSof, bool bNetUp)
	{
		if (m_Result != kWaiting) return m_Result;
		if (!m_bStarted) { m_bStarted = true; m_nFirst = nMs; }
		if (bSof) m_bSof = true;
		const bool bWatched = nMs - m_nFirst >= kMinWatchMs;
		if (bNetUp) m_Result = kUsbUp;
		else if (!m_bSof && nMs >= m_nQuiet && bWatched) m_Result = kNoHost;
		else if (nMs >= m_nWait && bWatched) m_Result = kNoDriver;
		return m_Result;
	}
	bool host_seen () const	{ return m_bSof; }

private:
	unsigned m_nQuiet, m_nWait;
	bool m_bSof = false, m_bStarted = false;
	unsigned m_nFirst = 0;
	Probe m_Result = kWaiting;
};

// ---- trust ---------------------------------------------------------------------------------------------------------

enum Iface { kIfNone, kIfEthernet, kIfWifi, kIfUsb };

// A connection skips the pairing code only when it ARRIVED on the USB-C cable (its ingress interface, never its
// address: any host can claim an address) and usbtrust= is on. With Circle's one stack on one device, a boot on the
// USB-C network has no other interface for anything to arrive on.
inline bool trusted (Iface ingress, bool bTrust)	{ return bTrust && ingress == kIfUsb; }

// ---- addresses -----------------------------------------------------------------------------------------------------

// The two MACs of the cable from the board's serial number: the Pi's end and the computer's (the descriptor's
// iMACAddress), both locally administered unicast (02:.. and 06:..), stable per board, different from each other.
inline void macs (uint64_t nSerial, uint8_t pi[6], uint8_t host[6])
{
	pi[0] = 0x02;
	for (int i = 1; i < 6; i++) pi[i] = (uint8_t) (nSerial >> (8 * (5 - i)));
	memcpy (host, pi, 6);
	host[0] = 0x06;
}

// iMACAddress's text: 12 upper-case hex digits
inline void mac_text (const uint8_t mac[6], char out[13])
{
	static const char h[] = "0123456789ABCDEF";
	for (int i = 0; i < 6; i++) { out[2 * i] = h[mac[i] >> 4]; out[2 * i + 1] = h[mac[i] & 15]; }
	out[12] = 0;
}

// ---- the descriptor block (CDC-NCM 1.0 with its interface association) ---------------------------------------------

struct DescOffsets { unsigned notify, in, out; };	// the three endpoint descriptors, from the block's start

constexpr unsigned kDescLen = 8 + 9 + 5 + 5 + 13 + 6 + 7 + 9 + 9 + 7 + 7;	// 85

// The block at p: interfaces nIface (communication, the notify EP) and nIface + 1 (data: alt 0 none, alt 1 bulk IN and
// OUT). Its length (kDescLen).
inline unsigned put_desc (uint8_t *p, uint8_t nIface, uint8_t strFunction, uint8_t strMac, uint8_t epNotify, uint8_t epIn,
			  uint8_t epOut, DescOffsets *pOff)
{
	unsigned n = 0;
#define USBNET_PUT(...)	do { const uint8_t b_[] = {__VA_ARGS__}; memcpy (p + n, b_, sizeof b_); n += sizeof b_; } while (0)
	USBNET_PUT (8, 0x0B, nIface, 2, 0x02, 0x0D, 0x00, strFunction);		// IAD: CDC, NCM
	USBNET_PUT (9, 0x04, nIface, 0, 1, 0x02, 0x0D, 0x00, strFunction);		// communication interface
	USBNET_PUT (5, 0x24, 0x00, 0x10, 0x01);					// header, CDC 1.10
	USBNET_PUT (5, 0x24, 0x06, nIface, (uint8_t) (nIface + 1));			// union
	USBNET_PUT (13, 0x24, 0x0F, strMac, 0, 0, 0, 0, 0xEA, 0x05, 0, 0, 0);	// Ethernet: no statistics, 1514, no filters
	USBNET_PUT (6, 0x24, 0x1A, 0x00, 0x01, 0x01);				// NCM 1.0: SetEthernetPacketFilter
	pOff->notify = n;
	USBNET_PUT (7, 0x05, epNotify, 0x03, 16, 0, 9);				// interrupt IN, 16 bytes, 32 ms at high speed
	USBNET_PUT (9, 0x04, (uint8_t) (nIface + 1), 0, 0, 0x0A, 0x00, 0x01, 0);	// data, alt 0: no endpoints
	USBNET_PUT (9, 0x04, (uint8_t) (nIface + 1), 1, 2, 0x0A, 0x00, 0x01, 0);	// data, alt 1: the link
	pOff->in = n;
	USBNET_PUT (7, 0x05, epIn, 0x02, 0x00, 0x02, 0);				// bulk IN, 512
	pOff->out = n;
	USBNET_PUT (7, 0x05, epOut, 0x02, 0x00, 0x02, 0);				// bulk OUT, 512
#undef USBNET_PUT
	return n;
}

}	// namespace usbnet

#endif
