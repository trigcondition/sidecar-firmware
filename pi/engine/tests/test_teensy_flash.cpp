// pi/engine/tests/test_teensy_flash.cpp - teensy_flash.cpp (the Pi writes the Teensy relay's program, docs/teensy-flash.md)
// on a fake Circle (shim_tf/): make build/test_teensy_flash
// The test plays the xHCI (each SET_REPORT's completion), the bootloader (its device coming and going) and the relay
// (on the link or not, its log), and steps the write as its task would.
#include "teensy_flash.h"
#include "teensy_hex.h"
#include "teensy_target.h"	// shim_tf's fake
#include "lib/sha256.h"
#include "check.h"

#include <circle/usb/usbfunction.h>
#include <circle/usb/usbstandardhub.h>
#include <circle/timer.h>
#include <cstdio>
#include <string>
#include <vector>

CUSBFunction *SidecarTeensyBootCreate (CUSBFunction *pParent);
namespace teensy_flash { void TestStep (void); }

using namespace teensyhex;

static CUSBDevice Dev;
static CUSBHostController Host;
static CUSBEndpoint Ep0;
static CUSBFunction Parent (&Dev, &Host, &Ep0);
static CUSBFunction *pBoot;

static void Ms (unsigned ms) { CTimer::s_now64 += (unsigned long long) ms * 1000; }
static void Step (unsigned n = 1) { while (n--) teensy_flash::TestStep (); }
static std::string State (void) { char b[1024]; CHECK (teensy_flash::StateJson (b, sizeof b) > 0); return b; }
static bool Has (const std::string &s, const char *p) { return s.find (p) != std::string::npos; }
static void BootUp (void) { pBoot = SidecarTeensyBootCreate (&Parent); CHECK (pBoot != 0 && pBoot->Configure ()); }
static void BootGone (void) { delete pBoot; pBoot = 0; }

static std::string Rec (unsigned type, unsigned addr, std::initializer_list<int> d)
{
	std::string r;
	unsigned s = (unsigned) d.size () + (addr >> 8) + (addr & 0xFF) + type;
	char h[32];
	snprintf (h, sizeof h, ":%02X%04X%02X", (unsigned) d.size (), addr, type); r = h;
	for (int b : d) { snprintf (h, sizeof h, "%02X", b); r += h; s += (unsigned) b; }
	snprintf (h, sizeof h, "%02X\r\n", (0x100 - (s & 0xFF)) & 0xFF); r += h;
	return r;
}

// a program in blocks 0, 4 and 9, uploaded in two chunks
static std::string Hex (void) { return Rec (4, 0, {0x60, 0x00}) + Rec (0, 0x0000, {1, 2}) + Rec (0, 0x1000, {3}) + Rec (0, 0x2400, {4}) + Rec (1, 0, {}); }
static void Upload (const std::string &h)
{
	uint32_t crc = core::lib::crc32 (0, h.data (), h.size ());
	fsweb::Result r;
	size_t half = h.size () / 2;
	teensy_flash::Put (0, h.size (), crc, (const uint8_t *) h.data (), (unsigned) half, &r);
	CHECK (r.status == 200 && strncmp (r.msg, "ok ", 3) == 0);
	teensy_flash::Put (half, h.size (), crc, (const uint8_t *) h.data () + half, (unsigned) (h.size () - half), &r);
	CHECK (r.status == 200 && strncmp (r.msg, "done ", 5) == 0 && strstr (r.msg, "3 blocks") != 0);
}

// the report at the controller's tail: its SET_REPORT and the block offset it carries
static CUSBRequest *Last (void) { CHECK (!Host.ep0.empty ()); return Host.ep0.back (); }
static uint32_t Off (CUSBRequest *u)
{
	CHECK (u->setup->bmRequestType == 0x21 && u->setup->bRequest == 9 && u->setup->wValue == 0x0200 && u->setup->wIndex == 0);
	CHECK (u->setup->wLength == kReport && u->len == kReport);
	for (unsigned i = 3; i < kHeader; i++) CHECK (u->buf[i] == 0);
	return u->buf[0] | u->buf[1] << 8 | u->buf[2] << 16;
}
// the boot report: the Teensy resets, its answer never comes; Circle ends it with the device (here at the end)
static std::vector<CUSBRequest *> Orphans;
static void Orphan (void) { Orphans.push_back (Last ()); Host.ep0.pop_back (); }
static void Done (CUSBRequest *u, unsigned st) { for (auto it = Host.ep0.begin (); it != Host.ep0.end (); ++it) if (*it == u) { Host.ep0.erase (it); break; } u->Complete (st); }

int main (void)
{
	fsweb::Result r;

	// ---- nothing uploaded, a bad file, no Teensy
	teensy_flash::Start ("", &r);
	CHECK (r.status == 409 && strstr (r.msg, "no program") != 0);
	{
		std::string bad = Rec (0, 0, {1}) + Rec (1, 0, {});
		teensy_flash::Put (0, bad.size (), core::lib::crc32 (0, bad.data (), bad.size ()), (const uint8_t *) bad.data (), (unsigned) bad.size (), &r);
		CHECK (r.status == 400 && strstr (r.msg, "outside") != 0);
		std::string h = Hex ();
		teensy_flash::Put (0, h.size (), 12345, (const uint8_t *) h.data (), (unsigned) h.size (), &r);
		CHECK (r.status == 400 && strstr (r.msg, "CRC-32") != 0);
		teensy_flash::Put (5, h.size (), 0, (const uint8_t *) h.data (), 3, &r);
		CHECK (r.status == 409);
	}
	Upload (Hex ());
	CHECK (Has (State (), "\"state\":\"ready\""));
	teensy_flash::Start ("", &r);
	CHECK (r.status == 409 && strstr (r.msg, "no Teensy") != 0);
	CHECK (Has (State (), "\"state\":\"ready\"") && Has (State (), "\"ms\":0"));

	// ---- A: through the relay; block 0 refused once (the erase), then the rest; the relay back with the build expected
	CTeensyTarget::up = true;
	teensy_flash::Start ("abc1234", &r);
	CHECK (r.status == 200 && CTeensyTarget::boot_requests == 1);
	CHECK (Has (State (), "\"state\":\"entering\""));
	{
		std::string h = Hex ();
		teensy_flash::Put (0, h.size (), 0, (const uint8_t *) h.data (), 2, &r);
		CHECK (r.status == 409);		// no upload while a write runs
		teensy_flash::Start ("", &r);
		CHECK (r.status == 409);
	}
	Step (); Ms (60);
	CTeensyTarget::up = false;		// the relay reboots into its bootloader
	BootUp ();
	Step ();				// -> writing
	CHECK (Has (State (), "\"state\":\"writing\""));
	Step ();				// block 0 out
	CHECK (Host.ep0.size () == 1 && Off (Last ()) == 0 && Last ()->buf[kHeader] == 1 && Last ()->buf[kHeader + 1] == 2 && Last ()->buf[kHeader + 2] == 0xFF);
	Step (3);				// pending: nothing new
	CHECK (Host.ep0.size () == 1);
	Ms (3000);
	Done (Last (), 0);			// refused
	Step ();
	CHECK (Ep0.resets == 1 && Host.ep0.empty () && Has (State (), "\"retries\":1"));
	Step ();				// within 10 ms: not yet again
	CHECK (Host.ep0.empty ());
	Ms (10); Step ();
	CHECK (Host.ep0.size () == 1 && Off (Last ()) == 0);
	Done (Last (), 1); Step ();
	CHECK (Host.ep0.size () == 1 && Off (Last ()) == 4 * kBlock && Last ()->buf[kHeader] == 3);
	Done (Last (), 1); Step ();
	CHECK (Off (Last ()) == 9 * kBlock && Last ()->buf[kHeader] == 4);
	Done (Last (), 1); Step ();		// the last block taken: the boot report
	CHECK (Host.ep0.size () == 1 && Off (Last ()) == kBootAddr);
	for (unsigned i = kHeader; i < kReport; i++) CHECK (Last ()->buf[i] == 0);
	CHECK (Has (State (), "\"state\":\"booting\"") && Has (State (), "\"sent\":3"));
	Orphan ();				// the Teensy reset: that report is never answered
	Step (); Ms (500);
	BootGone ();
	Step ();
	CHECK (Has (State (), "\"state\":\"booting\""));	// gone, the relay not yet back
	CTeensyTarget::up = true;
	Step ();
	CHECK (Has (State (), "\"state\":\"checking\"") && CTeensyTarget::log_requests == 1);
	Step ();				// not read yet
	CHECK (Has (State (), "\"state\":\"checking\""));
	CTeensyTarget::log = "usb2: up\nmirror relay up, protocol 1.00, build abc1234\n";
	CTeensyTarget::log_reads++;
	Step ();
	CHECK (Has (State (), "\"state\":\"done\"") && Has (State (), "\"build\":\"abc1234\""));

	// ---- B: the relay back with another build
	teensy_flash::Start ("abc9999", &r);
	CHECK (r.status == 200 && CTeensyTarget::boot_requests == 2);
	CTeensyTarget::up = false; BootUp ();
	Step (2);
	for (int k = 0; k < 3; k++) { Done (Last (), 1); Step (); }
	Orphan ();
	BootGone (); CTeensyTarget::up = true;
	Step ();
	CTeensyTarget::log_reads++;
	Step ();
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "abc1234, expected abc9999"));

	// ---- C: a Teensy already in its bootloader, no relay (the recovery path); no expect
	CTeensyTarget::up = false;
	BootUp ();
	teensy_flash::Start ("", &r);
	CHECK (r.status == 200 && strstr (r.msg, "in its bootloader") != 0 && CTeensyTarget::boot_requests == 2);
	Step (2);
	CHECK (Host.ep0.size () == 1 && Off (Last ()) == 0);

	// ---- D: block 0 never answered: given up after 45 s, its buffer not reused; its late answer is not the next one's
	CUSBRequest *pOld = Last ();
	Ms (44000); Step ();
	CHECK (Has (State (), "\"state\":\"writing\""));
	Ms (1500); Step ();
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "not answered"));
	u8 *pOldBuf = pOld->buf;
	teensy_flash::Start ("", &r);
	CHECK (r.status == 200);
	Step (2);
	CHECK (Host.ep0.size () == 2 && Last () != pOld && Last ()->buf != pOldBuf);
	Done (pOld, 1); Step ();		// the old one comes back late: still waiting for the new one
	CHECK (Has (State (), "\"sent\":0") && Host.ep0.size () == 1);

	// ---- E: the Teensy leaves its bootloader during the write
	BootGone ();
	Step ();
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "left its bootloader during"));
	Done (Last (), 0);			// its URB ends with the device: no crash, nothing counted

	// ---- F: the relay does not come back after the write
	BootUp ();
	teensy_flash::Start ("", &r);
	Step (2);
	for (int k = 0; k < 3; k++) { Done (Last (), 1); Step (); }
	Orphan ();
	BootGone ();
	Ms (19000); Step ();
	CHECK (Has (State (), "\"state\":\"booting\""));
	Ms (2000); Step ();
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "did not come back"));

	// ---- G: the relay back but no log read (relaydelay= longer than the wait)
	BootUp ();
	teensy_flash::Start ("", &r);
	Step (2);
	for (int k = 0; k < 3; k++) { Done (Last (), 1); Step (); }
	Orphan ();
	BootGone (); CTeensyTarget::up = true;
	Step ();
	Ms (21000); Step ();
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "log was not read"));

	// ---- H: the controller refuses the submit itself until block 0's time runs out
	CTeensyTarget::up = false;
	BootUp ();
	teensy_flash::Start ("", &r);
	Host.refuse = true;
	Step (2);
	CHECK (Host.ep0.empty () && Has (State (), "\"state\":\"writing\""));
	Ms (46000); Step ();
	CHECK (Has (State (), "\"state\":\"failed\""));
	Host.refuse = false;
	BootGone ();

	// ---- I: the Teensy on the Pi 4's VL805 hub (2109:3431 on a root port): its ports polled every 250 ms while the Teensy
	// moves (entering, booting), not while it writes or is checked; beside a streaming Rytm the hub's own report never came
	CUSBDevice HubDev, RelayDev;
	HubDev.desc.idVendor = 0x2109, HubDev.desc.idProduct = 0x3431, HubDev.root = (CUSBHCIRootPort *) &HubDev;
	CUSBStandardHub Hub;
	Hub.dev = &HubDev;
	RelayDev.hub = &Hub;
	CTeensyTarget::dev = &RelayDev;
	CTeensyTarget::up = true;
	teensy_flash::Start ("", &r);
	CHECK (r.status == 200);
	Step ();
	CHECK (Hub.polls == 0);			// not before 250 ms
	Ms (250); Step ();
	CHECK (Hub.polls == 1);
	Step (3);
	CHECK (Hub.polls == 1);			// once per 250 ms
	Ms (250); Step ();
	CHECK (Hub.polls == 2);
	CTeensyTarget::up = false;
	Dev.hub = &Hub;				// the bootloader on the same port
	BootUp ();
	Ms (250); Step ();			// -> writing: no poll
	CHECK (Has (State (), "\"state\":\"writing\"") && Hub.polls == 2);
	for (int k = 0; k < 3; k++) { Ms (250); Step (); CHECK (Hub.polls == 2); Done (Last (), 1); }
	Step ();
	CHECK (Has (State (), "\"state\":\"booting\""));
	Orphan ();
	Ms (250); Step ();
	CHECK (Hub.polls == 3);			// booting: polled again
	BootGone (); CTeensyTarget::up = true;
	Ms (250); Step ();
	CHECK (Has (State (), "\"state\":\"checking\""));
	unsigned nPolls = Hub.polls;
	Ms (1000); Step ();
	CHECK (Hub.polls == nPolls);		// checking: no poll
	CTeensyTarget::log = "mirror relay up, protocol 1.01, build abc1234\n";
	CTeensyTarget::log_reads++;
	Step ();
	CHECK (Has (State (), "\"state\":\"done\""));

	// ---- J: another hub (could be unplugged during the write): never polled; nor a Teensy on a root port
	HubDev.desc.idProduct = 0x2817;
	teensy_flash::Start ("", &r);
	for (int k = 0; k < 4; k++) { Ms (250); Step (); }
	CHECK (Hub.polls == nPolls);
	CTeensyTarget::up = false;
	Ms (15000); Step ();			// (no bootloader: the 15 s run out)
	CHECK (Has (State (), "\"state\":\"failed\"") && Has (State (), "did not come up"));
	Dev.hub = 0;
	CTeensyTarget::dev = 0;

	for (CUSBRequest *u : Orphans) u->Complete (0);	// after their device went: nothing counted, no crash
	CHECK (CUSBRequest::live () == 0);
	printf ("test_teensy_flash: PASS\n");
	return 0;
}
