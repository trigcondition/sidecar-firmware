// pi/engine/tests/test_stor_vol.cpp - stor_vol.cpp: the hot-plug state machine (mount once, a pull unmounts, an eject
// waits for the device to leave), the eject refusals, the 64 KB piece rule, sector 0's diagnosis, the JSON and lines.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "stor_vol.h"

using namespace stor;

namespace {
bool g_Present[kVols];
int g_MountResult[kVols];
int g_Mounts[kVols], g_Unmounts[kVols], g_Ejects[kVols], g_Diag[kVols];
bool g_EjectLeaves = false;	// the device leaves the tree at once on an eject (else at the next "plug-and-play")
char g_Log[4096];

bool fake_present (int v) { return g_Present[v]; }
int fake_mount (int v, Info *p)
{
	g_Mounts[v]++;
	if (g_MountResult[v]) return g_MountResult[v];
	p->fs = v == kNVME ? kExfat : kFat32, p->total = 64ull << 30, p->free = 60ull << 30, p->mount_us = 12000;
	p->free_known = v != kUSB3;		// USB3: a FAT32 without FSINFO (its free space counted only when asked)
	return 0;
}
void fake_unmount (int v) { g_Unmounts[v]++; }
bool fake_eject (int v)
{
	g_Ejects[v]++;
	if (g_EjectLeaves) g_Present[v] = false;
	return true;
}
void fake_describe (int v, Status *s)
{
	s->link = v == kNVME ? kPcie : kUsbSuper, s->gen = v == kNVME ? 2 : 0;
	strcpy (s->model, v == kNVME ? "TEST SSD 256GB" : "0781:5581");
}
void fake_diagnose (int v, char *w, int n) { g_Diag[v]++; snprintf (w, n, "GPT partition table: use the MBR scheme (FAT32)"); }
void fake_log (int lvl, const char *l)
{
	size_t n = strlen (g_Log);
	snprintf (g_Log + n, sizeof g_Log - n, "%c %s\n", lvl ? 'W' : 'N', l);
}
const Ops kOps = {fake_present, fake_mount, fake_unmount, fake_eject, fake_describe, fake_diagnose, fake_log};

void fresh (void)
{
	memset (g_Present, 0, sizeof g_Present);
	memset (g_MountResult, 0, sizeof g_MountResult);
	memset (g_Mounts, 0, sizeof g_Mounts), memset (g_Unmounts, 0, sizeof g_Unmounts);
	memset (g_Ejects, 0, sizeof g_Ejects), memset (g_Diag, 0, sizeof g_Diag);
	g_Log[0] = 0;
	g_EjectLeaves = false;
	reset ();
	set_ops (&kOps);
}

void sector (uint8_t *s) { memset (s, 0, 512); s[510] = 0x55, s[511] = 0xAA; }
void part (uint8_t *s, int k, uint8_t type, uint32_t lba)
{
	uint8_t *e = s + 446 + 16 * k;
	e[4] = type;
	e[8] = (uint8_t) lba, e[9] = (uint8_t) (lba >> 8), e[10] = (uint8_t) (lba >> 16), e[11] = (uint8_t) (lba >> 24);
}
void vbr (uint8_t *s, const char *oem) { sector (s); s[0] = 0xEB, s[1] = 0x76, s[2] = 0x90; memcpy (s + 3, oem, 8); }
}	// namespace

int main ()
{
	// ---- names
	const char *e;
	CHECK (parse_vol ("usb", &e) == kUSB && *e == 0);
	CHECK (parse_vol ("USB2:64", &e) == kUSB2 && *e == ':');
	CHECK (parse_vol ("nvme!", &e) == kNVME && *e == '!');
	CHECK (parse_vol ("sd", 0) == kSD && parse_vol ("US", 0) == -1 && parse_vol ("USBX", 0) == -1 && parse_vol ("", 0) == -1);
	CHECK (!strcmp (drive (kUSB3), "USB3:") && !strcmp (device (kNVME), "nvme1") && fatfs_index (kNVME) == 6 && fatfs_index (kUSB) == 2);

	// ---- hot-plug: a stick plugged is mounted once
	fresh ();
	poll ();
	CHECK (status (kUSB).state == kAbsent && g_Mounts[kUSB] == 0);
	g_Present[kUSB] = true;
	poll ();
	CHECK (mounted (kUSB) && g_Mounts[kUSB] == 1 && status (kUSB).fs == kFat32 && status (kUSB).link == kUsbSuper);
	CHECK (status (kUSB).total == 64ull << 30 && status (kUSB).mounts == 1 && status (kUSB).mount_us == 12000);
	CHECK (strstr (g_Log, "N stor: USB   mounted FAT32 60 GB free of 64 GB, USB 3 SuperSpeed (0781:5581) (mounted in 12 ms)"));
	poll (), poll ();
	CHECK (g_Mounts[kUSB] == 1);				// no remount while it stays
	// pulled while mounted: unmounted, counted, warned
	g_Present[kUSB] = false;
	poll ();
	CHECK (status (kUSB).state == kAbsent && g_Unmounts[kUSB] == 1 && status (kUSB).pulls == 1 && status (kUSB).mounts == 1);
	CHECK (strstr (g_Log, "W stor: USB was pulled while mounted"));
	CHECK (status (kUSB).total == 0 && status (kUSB).model[0] == 0);	// nothing of the old stick shown
	// plugged again: mounted again
	g_Present[kUSB] = true;
	poll ();
	CHECK (mounted (kUSB) && g_Mounts[kUSB] == 2 && status (kUSB).mounts == 2 && status (kUSB).pulls == 1);

	// ---- a stick that does not mount: diagnosed once, not retried until asked or replugged
	fresh ();
	g_Present[kUSB2] = true, g_MountResult[kUSB2] = 13;	// FR_NO_FILESYSTEM
	poll ();
	CHECK (status (kUSB2).state == kPresent && status (kUSB2).err == 13 && g_Diag[kUSB2] == 1 && strstr (status (kUSB2).why, "GPT"));
	CHECK (strstr (g_Log, "W stor: USB2 not mounted (FatFs error 13): GPT"));
	poll (), poll ();
	CHECK (g_Mounts[kUSB2] == 1);
	g_MountResult[kUSB2] = 0;				// (re-formatted on a computer, plugged in again - or mount_again)
	CHECK (mount_again (kUSB2) == 0);
	poll ();
	CHECK (mounted (kUSB2) && g_Mounts[kUSB2] == 2 && status (kUSB2).why[0] == 0 && status (kUSB2).err == 0);
	g_Present[kUSB2] = false, g_MountResult[kUSB2] = 13;
	poll ();
	CHECK (status (kUSB2).state == kAbsent && status (kUSB2).pulls == 1);
	g_Present[kUSB2] = true;					// a failed one pulled: removed, no unmount, no pull counted
	poll ();
	CHECK (status (kUSB2).state == kPresent);
	g_Present[kUSB2] = false;
	int um = g_Unmounts[kUSB2];
	poll ();
	CHECK (status (kUSB2).state == kAbsent && g_Unmounts[kUSB2] == um && status (kUSB2).pulls == 1);

	// ---- eject
	fresh ();
	CHECK (eject (kSD) && strstr (eject (kSD), "boot card"));
	CHECK (eject (kUSB) && strstr (eject (kUSB), "nothing"));
	CHECK (eject (7) && eject (-1));
	g_Present[kUSB] = true;
	poll ();
	set_busy (kUSB, true);
	CHECK (eject (kUSB) && strstr (eject (kUSB), "in use") && g_Ejects[kUSB] == 0 && mounted (kUSB));
	set_busy (kUSB, false);
	set_recording (kUSB, true);					// the recorder's destination, armed or recording
	CHECK (eject (kUSB) && strstr (eject (kUSB), "rec=stop") && g_Ejects[kUSB] == 0 && mounted (kUSB));
	set_recording (kUSB, false);
	CHECK (eject (kUSB) == 0 && g_Unmounts[kUSB] == 1 && g_Ejects[kUSB] == 1 && status (kUSB).state == kEjected);
	CHECK (status (kUSB).ejects == 1 && !status (kUSB).gone && strstr (g_Log, "USB ejected: safe to remove"));
	CHECK (eject (kUSB) && strstr (eject (kUSB), "already"));
	poll ();							// still in the tree (Circle removes it at its next plug-and-play pass):
	CHECK (status (kUSB).state == kEjected && g_Mounts[kUSB] == 1);	// not mounted again
	g_Present[kUSB] = false;
	poll ();
	CHECK (status (kUSB).state == kEjected && status (kUSB).gone);
	poll ();
	CHECK (status (kUSB).state == kEjected);			// "safe to remove" until a device shows
	g_Present[kUSB] = true;					// plugged again
	poll ();
	CHECK (mounted (kUSB) && g_Mounts[kUSB] == 2 && status (kUSB).ejects == 1);
	// eject of a stick that leaves at once, then mount_again with nothing there: stays absent
	g_EjectLeaves = true;
	CHECK (eject (kUSB) == 0 && status (kUSB).gone);
	CHECK (mount_again (kUSB) == 0);
	poll ();
	CHECK (status (kUSB).state == kAbsent && g_Mounts[kUSB] == 2);
	// an eject that leaves the device present (no RemoveDevice): mount_again remounts it
	fresh ();
	g_Present[kUSB3] = true;
	poll ();
	CHECK (eject (kUSB3) == 0 && !status (kUSB3).gone);
	CHECK (mount_again (kUSB3) == 0);
	poll ();
	CHECK (mounted (kUSB3) && g_Mounts[kUSB3] == 2 && !status (kUSB3).free_known);
	{
		char t[2048];
		line (kUSB3, t, sizeof t);
		CHECK (!strcmp (t, "USB3  mounted FAT32 64 GB, free space not counted (stor=free:USB3), USB 3 SuperSpeed (0781:5581)"));
		json_vols (t, sizeof t, false);
		CHECK (strstr (t, "{\"name\":\"USB3\",\"state\":\"mounted\",\"fs\":\"FAT32\",\"totalMB\":65536,\"freeMB\":null,"));
		set_free (kUSB3, 1ull << 30);
		CHECK (status (kUSB3).free_known);
	}
	// an unmountable stick can be ejected too (no unmount: it never mounted)
	fresh ();
	g_Present[kUSB] = true, g_MountResult[kUSB] = 13;
	poll ();
	CHECK (eject (kUSB) == 0 && g_Unmounts[kUSB] == 0 && g_Ejects[kUSB] == 1);

	// ---- NVME: unavailable on the Pi 4 (never polled), present at boot on the Pi 5
	fresh ();
	set_available (kNVME, false);
	g_Present[kNVME] = true;
	poll ();
	CHECK (status (kNVME).state == kUnavailable && g_Mounts[kNVME] == 0 && strstr (eject (kNVME), "not on this board"));
	set_available (kNVME, true);
	poll ();
	CHECK (mounted (kNVME) && status (kNVME).fs == kExfat && status (kNVME).link == kPcie && status (kNVME).gen == 2);
	CHECK (strstr (g_Log, "NVME  mounted exFAT 60 GB free of 64 GB, PCIe gen 2 x1 (TEST SSD 256GB)"));

	// ---- SD: listed from the platform's numbers, never mounted or ejected here
	set_sd (59ull << 30, 41ull << 30);
	CHECK (mounted (kSD) && status (kSD).link == kSdHs && status (kSD).fs == kFat32);
	set_sd (0, 0);
	CHECK (status (kSD).state == kAbsent);
	set_sd (59ull << 30, 41ull << 30);

	// ---- lines and JSON
	char b[2048];
	line (kSD, b, sizeof b);
	CHECK (!strcmp (b, "SD    mounted FAT32 41 GB free of 59 GB, SD HS 4-bit 50 MHz"));
	line (kUSB, b, sizeof b);
	CHECK (!strcmp (b, "USB   absent"));
	set_free (kNVME, 500ull << 20);
	line (kNVME, b, sizeof b);
	CHECK (strstr (b, "500 MB free of 64 GB"));
	int n = json_vols (b, sizeof b, false);
	CHECK (n == (int) strlen (b) && b[0] == '"' && b[n - 1] == ']');
	CHECK (strstr (b, "\"exfat\":false,\"vols\":[{\"name\":\"SD\",\"state\":\"mounted\",\"fs\":\"FAT32\",\"totalMB\":60416,\"freeMB\":41984"));
	CHECK (strstr (b, "{\"name\":\"NVME\",\"state\":\"mounted\",\"fs\":\"exFAT\",\"totalMB\":65536,\"freeMB\":500,\"link\":\"PCIe gen 2 x1\",\"model\":\"TEST SSD 256GB\""));
	set_why (kUSB, "a \"quoted\" note");
	line (kUSB, b, sizeof b);
	CHECK (!strcmp (b, "USB   absent: a \"quoted\" note"));
	json_vols (b, sizeof b, false);
	CHECK (strstr (b, "\"why\":\"a _quoted_ note\""));
	set_why (kUSB, "");
	n = json_vols (b, sizeof b, false);
	CHECK (json_vols (b, 40, true) == n - 1 && strlen (b) == 39);	// cut, the length counted ("true": one shorter)
	char m[24];
	mb_text (812ull << 20, m, sizeof m); CHECK (!strcmp (m, "812 MB"));
	mb_text (1536ull << 20, m, sizeof m); CHECK (!strcmp (m, "1.5 GB"));
	mb_text (64ull << 30, m, sizeof m); CHECK (!strcmp (m, "64 GB"));

	// ---- the 64 KB piece rule
	CHECK (piece (0x100000, 1 << 20) == 65536 && piece (0x100000, 4096) == 4096);
	CHECK (piece (0x10FFF0, 4096) == 16 && piece (0x10FFF0, 8) == 8);
	srand (7);
	for (int t = 0; t < 20000; t++)
	{
		uintptr_t a = ((uintptr_t) rand () << 8) ^ (uintptr_t) rand ();
		uint32_t len = (uint32_t) rand () % (3u << 20) + 1, sum = 0;
		while (sum < len)
		{
			uint32_t k = piece (a + sum, len - sum);
			CHECK (k > 0 && k <= kMaxPiece);
			CHECK (((a + sum) >> 16) == ((a + sum + k - 1) >> 16));	// never across a 64 KB boundary
			sum += k;
		}
		CHECK (sum == len);
	}
	for (uint32_t s = 0; s < (2u << 20); s += 65536) CHECK (piece (0x40000000 + s, (2u << 20) - s) == 65536);	// aligned: whole pieces

	// ---- sector 0
	uint8_t s0[512], v[512];
	uint32_t next;
	memset (s0, 0, 512);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "blank") && next == 0);
	sector (s0);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "empty partition table"));
	part (s0, 0, 0xEE, 1);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "GPT"));
	sector (s0), part (s0, 0, 0x07, 2048);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "exFAT or NTFS") && next == 2048);
	vbr (v, "EXFAT   ");
	CHECK (strstr (classify_sector0 (s0, v, &next), "exFAT: FAT32 only in this build") && next == 0);
	vbr (v, "NTFS    ");
	CHECK (strstr (classify_sector0 (s0, v, &next), "NTFS"));
	sector (s0), part (s0, 1, 0x0C, 8192);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "a FAT partition FatFs did not accept"));
	sector (s0), part (s0, 0, 0x83, 2048);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "Linux"));
	sector (s0), part (s0, 0, 0xAF, 2048);
	CHECK (strstr (classify_sector0 (s0, 0, &next), "HFS+"));
	sector (s0), part (s0, 0, 0x42, 2048);
	CHECK (!strcmp (classify_sector0 (s0, 0, &next), "partition type 42: not supported (format FAT32)"));
	vbr (s0, "EXFAT   ");
	CHECK (strstr (classify_sector0 (s0, 0, &next), "exFAT without a partition table"));

	printf ("test_stor_vol: ok\n");
	return 0;
}
