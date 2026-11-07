// pi/engine/tests/test_usbnet_cfg.cpp - usbnet_cfg.h: the USB-C network's cmdline.txt keys, the boot's choice (and
// that a Sidecar with no computer on USB-C keeps today's network), the trust rule, the MACs.
#include <cstdio>
#include <cstring>

#include "check.h"
#include "usbnet_cfg.h"

using namespace usbnet;

static Probe run (BootProbe &p, unsigned nSofAt, unsigned nUpAt, unsigned *pAt)
{
	for (unsigned t = 0; t <= 60000; t += 10)
	{
		Probe r = p.step (t, t >= nSofAt, t >= nUpAt);
		if (r != kWaiting) { *pAt = t; return r; }
	}
	return kWaiting;
}

int main ()
{
	// ---- keys
	Net n = kAuto;
	CHECK (parse_net ("usb", &n) && n == kUsb);
	CHECK (parse_net ("wifi", &n) && n == kWifi);
	CHECK (parse_net ("lan", &n) && n == kLan && parse_net ("ethernet", &n) && n == kLan && parse_net ("eth", &n) && n == kLan);
	CHECK (parse_net ("auto", &n) && n == kAuto);
	CHECK (!parse_net ("USB", &n) && !parse_net ("", &n) && !parse_net (0, &n) && n == kAuto);
	CHECK (!strcmp (net_name (kUsb), "usb") && !strcmp (net_name (kLan), "lan") && !strcmp (net_name (kAuto), "auto"));
	CHECK (parse_onoff ("1", false) && parse_onoff ("on", false) && !parse_onoff ("0", true) && !parse_onoff ("off", true));
	CHECK (parse_onoff (0, true) && !parse_onoff (0, false) && parse_onoff ("yes", true) && !parse_onoff ("yes", false));
	uint8_t ip[4] = {9, 9, 9, 9};
	CHECK (parse_ip ("172.31.77.1", ip) && ip[0] == 172 && ip[1] == 31 && ip[2] == 77 && ip[3] == 1);
	CHECK (parse_ip ("10.0.0.254", ip) && ip[3] == 254);
	const char *bad[] = {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1.2.3.0", "1.2.3.255", "0.1.2.3", "127.0.0.1", "224.0.0.1",
			     "255.255.255.255", "1.2.3.4 ", "1..3.4", "01234.1.1.1", "a.b.c.d", "1.2.3.-4"};
	for (const char *b : bad) { uint8_t t[4] = {7, 7, 7, 7}; CHECK (!parse_ip (b, t) && t[0] == 7); }
	uint8_t c[4];
	client_ip (kDefaultIp, c);
	CHECK (c[0] == 172 && c[1] == 31 && c[2] == 77 && c[3] == 2);
	const uint8_t two[4] = {10, 1, 2, 2};
	client_ip (two, c);
	CHECK (c[3] == 1 && c[2] == 2);

	// defaults: everything absent
	Options o = options (0, 0, 0, 0, 0, 0);
	CHECK (o.net == kAuto && o.usbnet && o.trust && !memcmp (o.ip, kDefaultIp, 4) && o.wait_s == 5 && o.quiet_ms == 700);
	CHECK (!o.bad_net && !o.bad_ip);
	o = options ("lan", "off", "10.9.8.1", "12", "300", "0");
	CHECK (o.net == kLan && !o.usbnet && !o.trust && o.ip[0] == 10 && o.ip[3] == 1 && o.wait_s == 12 && o.quiet_ms == 300);
	o = options ("bogus", "maybe", "300.1.1.1", "999", "1", "x");	// not understood: the defaults, said so
	CHECK (o.net == kAuto && o.bad_net && o.usbnet && o.bad_ip && !memcmp (o.ip, kDefaultIp, 4) && o.wait_s == 30 && o.quiet_ms == 100 && o.trust);
	o = options (0, 0, 0, "0", "x9", 0);
	CHECK (o.wait_s == 1 && o.quiet_ms == 700);

	// ---- the plan
	const char *why = 0;
	Inputs in;
	in.function = true;
	CHECK (plan (in, &why) == kProbe && why);
	in.net = kWifi;
	CHECK (plan (in, &why) == kToday && !strcmp (why, "net=wifi"));
	in.net = kLan;
	CHECK (plan (in, &why) == kToday);
	in.net = kUsb;
	CHECK (plan (in, &why) == kForced);
	in.wifi_trial = true;
	CHECK (plan (in, &why) == kForced);			// asked for: USB even then (the trial reverts, docs say so)
	in.net = kAuto;
	CHECK (plan (in, &why) == kToday);			// a Wi-Fi join being tried wins over the cable
	in.wifi_trial = false;
	in.link_tcp = true;
	CHECK (plan (in, &why) == kToday);
	in.link_tcp = false;
	in.function = false;					// usbnet=off, OVERBRIDGE, no gadget: never USB, even net=usb
	CHECK (plan (in, &why) == kToday);
	in.net = kUsb;
	CHECK (plan (in, &why) == kToday);

	// ---- the probe: no computer -> today's network after the quiet window
	unsigned at = 0;
	{
		BootProbe p (700, 5);
		CHECK (run (p, ~0u, ~0u, &at) == kNoHost && at == 700 && !p.host_seen ());
	}
	// a computer whose USB network starts at 1.2 s -> USB at 1.2 s
	{
		BootProbe p (700, 5);
		CHECK (run (p, 150, 1200, &at) == kUsbUp && at == 1200 && p.host_seen ());
	}
	// a computer on the bus that never starts a USB network (Windows 10 without the driver) -> today's after usbwait
	{
		BootProbe p (700, 5);
		CHECK (run (p, 150, ~0u, &at) == kNoDriver && at == 5000);
	}
	// SOF seen late (just before the quiet window ends) still counts as a computer
	{
		BootProbe p (700, 5);
		CHECK (run (p, 690, 3000, &at) == kUsbUp && at == 3000);
	}
	// the network already up when the wait starts (the overlap with the Wi-Fi chip's start): at once
	{
		BootProbe p (700, 5);
		CHECK (p.step (2500, true, true) == kUsbUp);
		CHECK (p.step (2600, false, false) == kUsbUp);	// decided once
	}
	// the first step after the quiet window (the Wi-Fi chip took longer): not decided on one sample, kMinWatchMs first
	{
		BootProbe p (700, 5);
		CHECK (p.step (1800, false, false) == kWaiting);
		CHECK (p.step (1850, false, false) == kWaiting);
		CHECK (p.step (1800 + kMinWatchMs, false, false) == kNoHost);
	}
	// ... and a bus reset at that moment (no SOF for a few ms), then start-of-frames: a computer, waited for
	{
		BootProbe p (700, 5);
		CHECK (p.step (1800, false, false) == kWaiting);
		CHECK (p.step (1830, true, false) == kWaiting && p.host_seen ());
		CHECK (p.step (2500, true, true) == kUsbUp);
	}
	// past usbwait at the first step too: watched first
	{
		BootProbe p (700, 5);
		CHECK (p.step (6000, true, false) == kWaiting);
		CHECK (p.step (6000 + kMinWatchMs, true, false) == kNoDriver);
	}

	// ---- trust: the cable only, never by default on another interface, usbtrust=0 turns it off
	CHECK (trusted (kIfUsb, true));
	CHECK (!trusted (kIfUsb, false));
	CHECK (!trusted (kIfWifi, true) && !trusted (kIfEthernet, true) && !trusted (kIfNone, true));

	// ---- MACs: locally administered unicast, stable, different ends
	uint8_t pi[6], host[6];
	macs (0x10000000abcdef12ull, pi, host);
	CHECK (pi[0] == 0x02 && host[0] == 0x06 && memcmp (pi + 1, host + 1, 5) == 0);
	CHECK ((pi[0] & 1) == 0 && (pi[0] & 2) && (host[0] & 1) == 0 && (host[0] & 2));
	CHECK (pi[1] == 0x00 && pi[2] == 0xab && pi[3] == 0xcd && pi[4] == 0xef && pi[5] == 0x12);
	char t[13];
	mac_text (host, t);
	CHECK (!strcmp (t, "0600ABCDEF12"));

	printf ("test_usbnet_cfg: OK\n");
	return 0;
}
