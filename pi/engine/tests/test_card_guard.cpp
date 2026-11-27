// test_card_guard.cpp - pi/engine/card_guard.h on the Mac: testcard=1 changes Sidecar's folders on the card and nothing
// else; Raspberry Pi OS's boot files, the root's own names and other ways of spelling them are refused; other drives are free.
#include "card_guard.h"
#include "check.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace cardguard;

int main ()
{
	// Sidecar's folders, the folder itself and inside it, in any case, with or without the drive
	for (const char *p : {"SD:/.rytm", "SD:/.sidecar/blackbox.log", "/.dt/project", "/.a4/project", "SD:/.a4", "/.sidecar/pair.txt", "/.rytm/projects/a/kit.txt", "SD:/Samples",
			      "SD:/Samples/pi-test/sine440.wav", "/samples/x.wav", "SD:/PLUGINS/a.rmp", "sd:/Recordings/t1/1.wav",
			      "/Bundles/a.sidecar.zip", "/PRESETS/x", "/MULTISAMPLE/a/b.wav", "/WAVETABLES/w", "/GRANULAR/g",
			      "/CHOP/c", "/NAM/n", "/JUNO/Factory/a.syx", "/DX7/a.syx", "/MACHINES/m", "/Plugins/.bak"})
	{
		CHECK (change_ok (p));
		CHECK (open_ok (p, true));
	}
	// Raspberry Pi OS's boot files and everything else at the root, as the file port, the Wi-Fi import and the pairing
	// recourse would name them
	for (const char *p : {"SD:/config.txt", "SD:/cmdline.txt", "SD:/kernel8.img", "SD:/kernel_2712.img", "SD:/kernel_2712.img.new",
			      "SD:/config.txt.bak", "SD:/config.txt.prev", "SD:/start4.elf", "SD:/fixup4.dat", "SD:/bootcode.bin",
			      "SD:/bcm2712-rpi-5-b.dtb", "SD:/overlays/README", "SD:/overlays", "SD:/issue.txt", "SD:/LICENCE.broadcom",
			      "SD:/tryboot.txt", "SD:/engine.img", "SD:/sdtest.tmp", "/config.txt", "config.txt", "/sidecar-paircode.txt",
			      "/sidecar-wifi.txt", "/sidecar-wifi.used", "/sidecar-wifi-error.txt", "SD:/usbtest.log.new",
			      "SD:/", "SD:", "/", "",
			      "SD:/Samples/../config.txt", "SD:/.rytm/../cmdline.txt", "/Samples/..", "SD:/Samples2/x", "SD:/Sample/x",
			      "SD:/.rytm2/x", "SD:/rytm/x", "SD:/SAMPLE~1/x", "0:/config.txt", "SD2:/config.txt", "./config.txt"})
	{
		CHECK (!change_ok (p));
		CHECK (!open_ok (p, true));
		CHECK (!open_ok (p, false));
	}
	// the test log: overwritten, never made
	CHECK (open_ok ("SD:/usbtest.log", false));
	CHECK (open_ok ("/USBTEST.LOG", false));
	CHECK (!open_ok ("SD:/usbtest.log", true));
	CHECK (!change_ok ("SD:/usbtest.log"));
	// other drives are not this card
	for (const char *p : {"USB:/storbench.tmp", "USB2:/config.txt", "USB3:/a/b", "NVME:/config.txt", "NVME:/overlays/x"})
	{
		CHECK (change_ok (p));
		CHECK (open_ok (p, true));
	}
	// f_rename: both names; the new one has no drive of its own
	CHECK (rename_ok ("SD:/.sidecar/pair.txt.new", "/.sidecar/pair.txt"));
	CHECK (rename_ok ("/Samples/a", "/Samples/b"));
	CHECK (rename_ok ("USB:/config.txt", "/x.txt"));
	CHECK (!rename_ok ("SD:/config.txt", "/.rytm/x"));
	CHECK (!rename_ok ("SD:/.rytm/x", "/config.txt"));
	CHECK (!rename_ok ("SD:/kernel_2712.img.new", "/kernel_2712.img"));
	CHECK (!rename_ok ("SD:/sidecar-wifi.txt", "/sidecar-wifi.used"));
	printf ("test_card_guard: ok\n");
	return 0;
}
